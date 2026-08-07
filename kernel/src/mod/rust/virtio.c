#include <drivers/virtio/queue.h>
#include <drivers/virtio/virtio.h>
#include <libs/klibc.h>
#include <mod/rust/api.h>
#include <task/task.h>

#define NA_VIRTIO_F_RING_INDIRECT_DESC (1ULL << 28)
#define NA_VIRTIO_F_RING_EVENT_IDX (1ULL << 29)
#define NA_VIRTIO_F_VERSION_1 (1ULL << 32)
#define NA_VIRTIO_RESPONSE_WATCHDOG_NS 10000000LL
#define NA_VIRTIO_CONFIG_POLL_NS 1000000000LL

typedef struct na_virtio_registration na_virtio_registration_t;

struct na_virtio_device {
    virtio_driver_t *driver;
    na_virtio_registration_t *registration;
    uint64_t features;
    wait_queue_head_t config_wait;
    bool config_pending;
    bool config_worker_started;
    void (*config_handler)(void *context);
    void *config_context;
};

struct na_virtio_queue {
    na_virtio_device_t *device;
    virtqueue_t *queue;
    wait_mutex_t lock;
    void *request_buffer;
    size_t request_capacity;
    void *extra_buffer;
    size_t extra_capacity;
    void *response_buffer;
    size_t response_capacity;
};

struct na_virtio_registration {
    virtio_device_driver_t driver;
    na_virtio_driver_ops_t ops;
    uint64_t supported_features;
};

static void na_virtio_config_irq(void *opaque, uint8_t isr_status) {
    na_virtio_device_t *device = opaque;
    if (!device || !(isr_status & 0x2))
        return;

    __atomic_store_n(&device->config_pending, true, __ATOMIC_RELEASE);
    wait_queue_wake_all(&device->config_wait, 0, EOK);
}

static void na_virtio_config_worker(uint64_t arg) {
    na_virtio_device_t *device = (na_virtio_device_t *)arg;

    for (;;) {
        bool pending = __atomic_exchange_n(&device->config_pending, false,
                                           __ATOMIC_ACQ_REL);
        if (!pending) {
            wait_queue_entry_t wait;
            task_prepare_block(current_task);
            wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
            wait_queue_add(&device->config_wait, &wait);
            pending = __atomic_exchange_n(&device->config_pending, false,
                                          __ATOMIC_ACQ_REL);
            if (!pending) {
                int64_t timeout =
                    virtio_driver_supports_interrupts(device->driver)
                        ? -1
                        : NA_VIRTIO_CONFIG_POLL_NS;
                (void)task_block(current_task, TASK_BLOCKING, timeout,
                                 "virtio_config");
            } else {
                task_cancel_block_prepare(current_task);
            }
            wait_queue_remove(&device->config_wait, &wait);
            task_cancel_block_prepare(current_task);
        }

        if (device->config_handler)
            device->config_handler(device->config_context);
    }
}

static int na_virtio_probe(virtio_driver_t *driver) {
    if (!driver || !driver->bound_driver || !driver->bound_driver->data)
        return -EINVAL;

    na_virtio_registration_t *registration = driver->bound_driver->data;
    na_virtio_device_t *device = calloc(1, sizeof(*device));
    if (!device)
        return -ENOMEM;

    device->driver = driver;
    device->registration = registration;
    wait_queue_init(&device->config_wait);
    device->features = virtio_begin_init(
        driver, registration->supported_features | NA_VIRTIO_F_VERSION_1);
    if (!device->features) {
        free(device);
        return -ENODEV;
    }

    if (virtio_driver_supports_interrupts(driver))
        virtio_driver_set_interrupt_handler(driver, na_virtio_config_irq,
                                            device);

    int result = registration->ops.probe(registration->ops.context, device);
    if (result < 0)
        free(device);
    return result;
}

int na_virtio_driver_register(const char *name, uint32_t device_type,
                              uint64_t supported_features,
                              const na_virtio_driver_ops_t *ops) {
    if (!name || !ops || !ops->probe)
        return -EINVAL;

    na_virtio_registration_t *registration = calloc(1, sizeof(*registration));
    if (!registration)
        return -ENOMEM;

    registration->ops = *ops;
    registration->supported_features = supported_features;
    registration->driver.name = name;
    registration->driver.device_type = (virtio_device_type_t)device_type;
    registration->driver.data = registration;
    registration->driver.probe = na_virtio_probe;
    return virtio_register_device_driver(&registration->driver);
}

uint64_t na_virtio_device_features(const na_virtio_device_t *device) {
    return device ? device->features : 0;
}

void na_virtio_device_finish(na_virtio_device_t *device) {
    if (device)
        virtio_finish_init(device->driver);
}

uint32_t na_virtio_device_config_read(const na_virtio_device_t *device,
                                      uint32_t offset) {
    return device ? virtio_driver_read_config_u32(device->driver, offset) : 0;
}

void na_virtio_device_config_write(na_virtio_device_t *device, uint32_t offset,
                                   uint32_t value) {
    if (device)
        virtio_driver_write_config_u32(device->driver, offset, value);
}

pci_device_t *na_virtio_device_pci(const na_virtio_device_t *device) {
    return device ? virtio_driver_parent_native(device->driver) : NULL;
}

int na_virtio_device_queue(na_virtio_device_t *device, uint16_t queue_index,
                           na_virtio_queue_t **output) {
    if (!device || !output)
        return -EINVAL;

    na_virtio_queue_t *queue = calloc(1, sizeof(*queue));
    if (!queue)
        return -ENOMEM;
    queue->device = device;
    queue->queue =
        virt_queue_new(device->driver, queue_index,
                       !!(device->features & NA_VIRTIO_F_RING_INDIRECT_DESC),
                       !!(device->features & NA_VIRTIO_F_RING_EVENT_IDX));
    if (!queue->queue) {
        free(queue);
        return -ENODEV;
    }
    wait_mutex_init(&queue->lock);
    *output = queue;
    return 0;
}

static int na_virtio_buffer_reserve(void **buffer, size_t *capacity,
                                    size_t required) {
    if (required == 0)
        return 0;
    if (*capacity >= required)
        return 0;

    void *replacement = alloc_frames_bytes(required);
    if (!replacement)
        return -ENOMEM;
    if (*buffer)
        free_frames_bytes(*buffer, *capacity);
    *buffer = replacement;
    *capacity = required;
    return 0;
}

static uint16_t na_virtio_wait_used(na_virtio_queue_t *queue,
                                    uint32_t *used_len) {
    for (;;) {
        uint16_t used = virt_queue_get_used_buf(queue->queue, used_len);
        if (used != 0xFFFF)
            return used;

        virtio_driver_t *driver = queue->device->driver;
        if (!current_task || !virtio_driver_supports_interrupts(driver)) {
            if (current_task)
                schedule(0);
            else
                arch_pause();
            continue;
        }

        uint64_t observed = virtio_driver_interrupt_seq(driver);
        used = virt_queue_get_used_buf(queue->queue, used_len);
        if (used != 0xFFFF)
            return used;
        (void)virtio_driver_wait_interrupt(driver, observed,
                                           NA_VIRTIO_RESPONSE_WATCHDOG_NS);
    }
}

int na_virtio_queue_submit(na_virtio_queue_t *queue, const void *request,
                           size_t request_size, const void *extra,
                           size_t extra_size, void *response,
                           size_t response_size) {
    if (!queue || !request || request_size == 0 || !response ||
        response_size == 0 || (extra_size && !extra))
        return -EINVAL;

    int result = na_virtio_buffer_reserve(
        &queue->request_buffer, &queue->request_capacity, request_size);
    if (result < 0)
        return result;
    result = na_virtio_buffer_reserve(&queue->extra_buffer,
                                      &queue->extra_capacity, extra_size);
    if (result < 0)
        return result;
    result = na_virtio_buffer_reserve(&queue->response_buffer,
                                      &queue->response_capacity, response_size);
    if (result < 0)
        return result;

    memcpy(queue->request_buffer, request, request_size);
    if (extra_size)
        memcpy(queue->extra_buffer, extra, extra_size);
    memset(queue->response_buffer, 0, response_size);

    virtio_buffer_t buffers[3];
    bool writable[3] = {false, false, false};
    uint16_t count = 0;
    buffers[count++] =
        (virtio_buffer_t){(uint64_t)queue->request_buffer, request_size};
    if (extra_size)
        buffers[count++] =
            (virtio_buffer_t){(uint64_t)queue->extra_buffer, extra_size};
    buffers[count] =
        (virtio_buffer_t){(uint64_t)queue->response_buffer, response_size};
    writable[count++] = true;

    dma_sync_cpu_to_device(queue->request_buffer, request_size);
    if (extra_size)
        dma_sync_cpu_to_device(queue->extra_buffer, extra_size);
    dma_sync_cpu_to_device(queue->response_buffer, response_size);

    wait_mutex_lock(&queue->lock);
    uint16_t descriptor =
        virt_queue_add_buf(queue->queue, buffers, count, writable);
    if (descriptor == 0xFFFF) {
        wait_mutex_unlock(&queue->lock);
        return -EIO;
    }
    virt_queue_submit_buf(queue->queue, descriptor);
    virt_queue_notify(queue->device->driver, queue->queue);

    uint32_t used_len = 0;
    uint16_t used = na_virtio_wait_used(queue, &used_len);
    virt_queue_free_desc(queue->queue, used);
    wait_mutex_unlock(&queue->lock);

    dma_sync_device_to_cpu(queue->response_buffer, response_size);
    memcpy(response, queue->response_buffer, response_size);
    return 0;
}

void na_virtio_device_set_config_handler(na_virtio_device_t *device,
                                         void *context,
                                         void (*handler)(void *context)) {
    if (!device)
        return;
    device->config_context = context;
    device->config_handler = handler;
    if (!handler || device->config_worker_started)
        return;
    device->config_worker_started = true;
    task_create("virtio_config", na_virtio_config_worker, (uint64_t)device,
                KTHREAD_PRIORITY);
}
