#include <irq/softirq.h>

static softirq_handler_t softirq_handlers[SOFTIRQ_MAX] = {0};
static uint64_t softirq_pending = 0;

void softirq_init(void) {
    memset(softirq_handlers, 0, sizeof(softirq_handlers));
    __atomic_store_n(&softirq_pending, 0, __ATOMIC_RELEASE);
}

void softirq_register(softirq_id_t id, softirq_handler_t handler) {
    if (id >= SOFTIRQ_MAX) {
        return;
    }

    __atomic_store_n(&softirq_handlers[id], handler, __ATOMIC_RELEASE);
}

bool softirq_raise(softirq_id_t id) {
    if (id >= SOFTIRQ_MAX) {
        return false;
    }

    uint64_t old_pending =
        __atomic_fetch_or(&softirq_pending, 1ULL << id, __ATOMIC_ACQ_REL);

    return old_pending == 0;
}

bool softirq_has_pending(void) {
    return __atomic_load_n(&softirq_pending, __ATOMIC_ACQUIRE) != 0;
}

void softirq_handle_pending(void) {
    uint64_t pending =
        __atomic_exchange_n(&softirq_pending, 0, __ATOMIC_ACQ_REL);

    for (uint32_t id = 0; id < SOFTIRQ_MAX; id++) {
        if (!(pending & (1ULL << id))) {
            continue;
        }

        softirq_handler_t handler =
            __atomic_load_n(&softirq_handlers[id], __ATOMIC_ACQUIRE);
        if (handler) {
            handler();
        }
    }
}
