#include <limits.h>
#include <mm/mm.h>
#include <mod/rust/api.h>
#include <net/netdev.h>

struct na_net_registration {
    netdev_t *device;
    na_net_device_ops_t ops;
};

static int na_net_transmit(void *context, void *data, uint32_t size) {
    na_net_registration_t *registration = context;

    if (!registration || !registration->ops.transmit)
        return -ENODEV;
    return registration->ops.transmit(registration->ops.context, data, size);
}

static int na_net_receive(void *context, void *data, uint32_t size) {
    na_net_registration_t *registration = context;

    if (!registration || !registration->ops.receive)
        return -ENODEV;
    return registration->ops.receive(registration->ops.context, data, size);
}

int na_net_device_register(const na_net_device_config_t *config,
                           const na_net_device_ops_t *ops,
                           na_net_registration_t **output) {
    na_net_registration_t *registration;

    if (!config || !ops || !ops->transmit || !ops->receive || !output ||
        !config->mtu || config->kind > NETDEV_TYPE_WIFI)
        return -EINVAL;
    *output = NULL;

    registration = calloc(1, sizeof(*registration));
    if (!registration)
        return -ENOMEM;
    registration->ops = *ops;
    registration->device = netdev_register_full(
        config->name, config->kind, registration, config->mac, config->mtu,
        na_net_transmit, na_net_receive, NULL);
    if (!registration->device) {
        free(registration);
        return -ENOSPC;
    }

    *output = registration;
    return 0;
}

int na_net_device_unregister(na_net_registration_t *registration) {
    int status;

    if (!registration || !registration->device)
        return -EINVAL;
    status = netdev_unregister(registration->device);
    if (status < 0)
        return status;
    registration->device = NULL;
    free(registration);
    return 0;
}

int na_net_device_set_link(na_net_registration_t *registration, bool link_up) {
    if (!registration || !registration->device)
        return -EINVAL;
    return netdev_set_link_state(registration->device, link_up);
}
