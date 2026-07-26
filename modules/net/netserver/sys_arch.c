#include "netserver_internal.h"
#include <init/callbacks.h>

typedef struct lwip_thread_bootstrap {
    lwip_thread_fn fn;
    void *arg;
} lwip_thread_bootstrap_t;

typedef struct lwip_thread_sem_entry {
    task_t *task;
    sys_sem_t sem;
    struct lwip_thread_sem_entry *next;
} lwip_thread_sem_entry_t;

typedef struct lwip_thread_sem_bucket {
    spinlock_t lock;
    lwip_thread_sem_entry_t *head;
} lwip_thread_sem_bucket_t;

#define LWIP_THREAD_SEM_BUCKET_COUNT 64U

static spinlock_t naos_lwip_protect_lock = SPIN_INIT;
static uintptr_t naos_lwip_protect_owner = 0;
static uint32_t naos_lwip_protect_depth = 0;
static lwip_thread_sem_bucket_t
    naos_lwip_thread_sem_buckets[LWIP_THREAD_SEM_BUCKET_COUNT];
static spinlock_t naos_lwip_thread_sem_callback_lock = SPIN_INIT;
static bool naos_lwip_thread_sem_callback_registered = false;
static spinlock_t naos_lwip_mbox_lifecycle_lock = SPIN_INIT;

static uintptr_t naos_lwip_task_owner_id(task_t *task) {
    return task ? (((uintptr_t)task << 1) | 1UL) : 0;
}

static uintptr_t naos_lwip_protect_owner_id(void) {
    task_t *task = current_task;

    if (task) {
        return naos_lwip_task_owner_id(task);
    }

    return ((uintptr_t)(current_cpu_id + 1) << 1);
}

static lwip_thread_sem_bucket_t *naos_lwip_thread_sem_bucket(task_t *task) {
    uintptr_t key = (uintptr_t)task >> 6;

    return &naos_lwip_thread_sem_buckets[key % LWIP_THREAD_SEM_BUCKET_COUNT];
}

static lwip_thread_sem_entry_t *
naos_lwip_thread_sem_find_locked(lwip_thread_sem_bucket_t *bucket,
                                 task_t *task) {
    lwip_thread_sem_entry_t *entry = bucket ? bucket->head : NULL;

    while (entry) {
        if (entry->task == task)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static void naos_lwip_thread_sem_free_task(task_t *task) {
    lwip_thread_sem_bucket_t *bucket = NULL;
    lwip_thread_sem_entry_t *entry = NULL;
    lwip_thread_sem_entry_t *prev = NULL;

    if (!task)
        return;

    bucket = naos_lwip_thread_sem_bucket(task);
    spin_lock(&bucket->lock);
    entry = bucket->head;
    while (entry && entry->task != task) {
        prev = entry;
        entry = entry->next;
    }
    if (entry) {
        if (prev)
            prev->next = entry->next;
        else
            bucket->head = entry->next;
    }
    spin_unlock(&bucket->lock);

    if (entry) {
        sys_sem_free(&entry->sem);
        free(entry);
    }
}

static int naos_lwip_thread_sem_on_exit(task_t *task) {
    naos_lwip_thread_sem_free_task(task);
    return 0;
}

void naos_lwip_thread_sem_registry_init(void) {
    spin_lock(&naos_lwip_thread_sem_callback_lock);
    if (naos_lwip_thread_sem_callback_registered) {
        spin_unlock(&naos_lwip_thread_sem_callback_lock);
        return;
    }
    naos_lwip_thread_sem_callback_registered = true;
    spin_unlock(&naos_lwip_thread_sem_callback_lock);

    regist_on_exit_task_callback(naos_lwip_thread_sem_on_exit);
}

sys_sem_t *naos_lwip_thread_sem_get(void) {
    task_t *task = current_task;
    lwip_thread_sem_bucket_t *bucket = NULL;
    lwip_thread_sem_entry_t *entry = NULL;
    lwip_thread_sem_entry_t *created = NULL;

    if (!task)
        return NULL;

    bucket = naos_lwip_thread_sem_bucket(task);
    spin_lock(&bucket->lock);
    entry = naos_lwip_thread_sem_find_locked(bucket, task);
    spin_unlock(&bucket->lock);
    if (entry)
        return &entry->sem;

    created = calloc(1, sizeof(*created));
    if (!created || sys_sem_new(&created->sem, 0) != ERR_OK) {
        free(created);
        return NULL;
    }
    created->task = task;

    spin_lock(&bucket->lock);
    entry = naos_lwip_thread_sem_find_locked(bucket, task);
    if (!entry) {
        created->next = bucket->head;
        bucket->head = created;
        entry = created;
        created = NULL;
    }
    spin_unlock(&bucket->lock);

    if (created) {
        sys_sem_free(&created->sem);
        free(created);
    }
    return &entry->sem;
}

void naos_lwip_thread_sem_alloc(void) { (void)naos_lwip_thread_sem_get(); }

void naos_lwip_thread_sem_free(void) {
    naos_lwip_thread_sem_free_task(current_task);
}

static bool naos_lwip_sem_trywait(sys_sem_t sem) {
    bool acquired = false;

    if (!sem || !sem->valid) {
        return false;
    }

    spin_lock(&sem->sem.lock);
    if (sem->sem.cnt > 0) {
        sem->sem.cnt--;
        acquired = true;
    }
    spin_unlock(&sem->sem.lock);

    return acquired;
}

static void naos_lwip_sem_wait_enqueue_locked(sys_sem_t sem,
                                              wait_node_t *node) {
    if (!sem || !node || node->queued) {
        return;
    }

    node->next = NULL;
    node->queued = true;
    if (sem->wait_tail) {
        sem->wait_tail->next = node;
    } else {
        sem->wait_head = node;
    }
    sem->wait_tail = node;
}

static void naos_lwip_sem_wait_remove_locked(sys_sem_t sem,
                                             wait_node_t *target) {
    wait_node_t *prev = NULL;
    wait_node_t *curr = NULL;

    if (!sem || !target || !target->queued) {
        return;
    }

    curr = sem->wait_head;
    while (curr) {
        if (curr == target) {
            if (prev) {
                prev->next = curr->next;
            } else {
                sem->wait_head = curr->next;
            }
            if (sem->wait_tail == curr) {
                sem->wait_tail = prev;
            }
            curr->next = NULL;
            curr->queued = false;
            return;
        }
        prev = curr;
        curr = curr->next;
    }

    target->next = NULL;
    target->queued = false;
}

static wait_node_t *naos_lwip_sem_wait_dequeue_locked(sys_sem_t sem) {
    wait_node_t *node = NULL;

    if (!sem || !sem->wait_head) {
        return NULL;
    }

    node = sem->wait_head;
    sem->wait_head = node->next;
    if (!sem->wait_head) {
        sem->wait_tail = NULL;
    }
    node->next = NULL;
    node->queued = false;
    return node;
}

static void naos_lwip_mutex_wait_enqueue_locked(sys_mutex_t mutex,
                                                wait_node_t *node) {
    if (!mutex || !node || node->queued) {
        return;
    }

    node->next = NULL;
    node->queued = true;
    if (mutex->wait_tail) {
        mutex->wait_tail->next = node;
    } else {
        mutex->wait_head = node;
    }
    mutex->wait_tail = node;
}

static void naos_lwip_mutex_wait_remove_locked(sys_mutex_t mutex,
                                               wait_node_t *target) {
    wait_node_t *prev = NULL;
    wait_node_t *curr = NULL;

    if (!mutex || !target || !target->queued) {
        return;
    }

    curr = mutex->wait_head;
    while (curr) {
        if (curr == target) {
            if (prev) {
                prev->next = curr->next;
            } else {
                mutex->wait_head = curr->next;
            }
            if (mutex->wait_tail == curr) {
                mutex->wait_tail = prev;
            }
            curr->next = NULL;
            curr->queued = false;
            return;
        }
        prev = curr;
        curr = curr->next;
    }

    target->next = NULL;
    target->queued = false;
}

static wait_node_t *naos_lwip_mutex_wait_dequeue_locked(sys_mutex_t mutex) {
    wait_node_t *node = NULL;

    if (!mutex || !mutex->wait_head) {
        return NULL;
    }

    node = mutex->wait_head;
    mutex->wait_head = node->next;
    if (!mutex->wait_head) {
        mutex->wait_tail = NULL;
    }
    node->next = NULL;
    node->queued = false;
    return node;
}

sys_prot_t naos_lwip_protect_enter(void) {
    uintptr_t owner = naos_lwip_protect_owner_id();
    sys_prot_t level = naos_lwip_protect_depth;

    if (naos_lwip_protect_depth && naos_lwip_protect_owner == owner) {
        naos_lwip_protect_depth++;
        return level;
    }

    spin_lock(&naos_lwip_protect_lock);
    naos_lwip_protect_owner = owner;
    naos_lwip_protect_depth = 1;
    return 0;
}

void naos_lwip_protect_leave(sys_prot_t level) {
    (void)level;

    uintptr_t owner = naos_lwip_protect_owner_id();

    if (!naos_lwip_protect_depth || naos_lwip_protect_owner != owner) {
        return;
    }

    naos_lwip_protect_depth--;
    if (naos_lwip_protect_depth == 0) {
        naos_lwip_protect_owner = 0;
        spin_unlock(&naos_lwip_protect_lock);
    }
}

void sys_init(void) {}

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
    sys_sem_t created = calloc(1, sizeof(*created));
    if (!created) {
        return ERR_MEM;
    }

    spin_init(&created->sem.lock);
    created->sem.cnt = count;
    created->sem.invalid = false;
    created->wait_head = NULL;
    created->wait_tail = NULL;
    created->valid = true;
    *sem = created;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem) {
    wait_node_t *node = NULL;
    task_t *waiter = NULL;

    if (!sem || !*sem || !(*sem)->valid) {
        return;
    }

    spin_lock(&(*sem)->sem.lock);
    (*sem)->sem.cnt++;
    while ((node = naos_lwip_sem_wait_dequeue_locked(*sem))) {
        waiter = node->task;
        node->task = NULL;
        if (waiter && waiter->state != TASK_DIED) {
            break;
        }
        waiter = NULL;
    }
    spin_unlock(&(*sem)->sem.lock);

    if (waiter) {
        task_unblock(waiter, EOK);
    }
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
    uint64_t start = nano_time();
    uint64_t timeout_ns = timeout ? (uint64_t)timeout * 1000000ULL : 0;
    sys_sem_t s = NULL;
    wait_node_t wait_node;

    if (!sem || !*sem || !(*sem)->valid) {
        return SYS_ARCH_TIMEOUT;
    }
    s = *sem;
    memset(&wait_node, 0, sizeof(wait_node));
    wait_node.task = current_task;

    for (;;) {
        uint64_t now = 0;
        int64_t block_ns = -1;
        int reason = EOK;

        task_prepare_block(current_task);
        spin_lock(&s->sem.lock);
        if (!s->valid) {
            naos_lwip_sem_wait_remove_locked(s, &wait_node);
            spin_unlock(&s->sem.lock);
            task_cancel_block_prepare(current_task);
            return SYS_ARCH_TIMEOUT;
        }
        if (s->sem.cnt > 0) {
            s->sem.cnt--;
            naos_lwip_sem_wait_remove_locked(s, &wait_node);
            spin_unlock(&s->sem.lock);
            task_cancel_block_prepare(current_task);
            break;
        }

        now = nano_time();
        if (timeout_ns && now - start >= timeout_ns) {
            naos_lwip_sem_wait_remove_locked(s, &wait_node);
            spin_unlock(&s->sem.lock);
            task_cancel_block_prepare(current_task);
            return SYS_ARCH_TIMEOUT;
        }

        wait_node.task = current_task;
        naos_lwip_sem_wait_enqueue_locked(s, &wait_node);
        if (timeout_ns) {
            uint64_t elapsed = now - start;
            block_ns = (int64_t)(timeout_ns - elapsed);
        }
        spin_unlock(&s->sem.lock);

        reason =
            task_block(current_task, TASK_BLOCKING, block_ns, "lwip_sem_wait");
        if (reason == ETIMEDOUT && timeout_ns) {
            bool timed_out = false;

            spin_lock(&s->sem.lock);
            if (!s->valid) {
                naos_lwip_sem_wait_remove_locked(s, &wait_node);
                timed_out = true;
            } else if (wait_node.queued) {
                naos_lwip_sem_wait_remove_locked(s, &wait_node);
                timed_out = true;
            } else if (s->sem.cnt > 0) {
                /* A signal selected this waiter concurrently with the timeout.
                 * Consume that signal here instead of leaking it into the
                 * caller's next netconn operation. */
                s->sem.cnt--;
            } else {
                timed_out = true;
            }
            spin_unlock(&s->sem.lock);
            task_cancel_block_prepare(current_task);
            if (timed_out)
                return SYS_ARCH_TIMEOUT;
            break;
        }
    }

    if (!timeout) {
        return 0;
    }

    return (u32_t)((nano_time() - start) / 1000000ULL);
}

void sys_sem_free(sys_sem_t *sem) {
    wait_node_t *node = NULL;
    bool has_waiters = false;

    if (!sem || !*sem) {
        return;
    }

    spin_lock(&(*sem)->sem.lock);
    has_waiters = (*sem)->wait_head != NULL;
    (*sem)->valid = false;
    node = naos_lwip_sem_wait_dequeue_locked(*sem);
    spin_unlock(&(*sem)->sem.lock);

    while (node) {
        task_t *waiter = node->task;
        node->task = NULL;
        if (waiter && waiter->state != TASK_DIED) {
            task_unblock(waiter, EOK);
        }

        spin_lock(&(*sem)->sem.lock);
        node = naos_lwip_sem_wait_dequeue_locked(*sem);
        spin_unlock(&(*sem)->sem.lock);
    }

    if (has_waiters) {
        *sem = NULL;
        return;
    }

    free(*sem);
    *sem = NULL;
}

int sys_sem_valid(sys_sem_t *sem) { return sem && *sem && (*sem)->valid; }

void sys_sem_set_invalid(sys_sem_t *sem) {
    if (sem) {
        *sem = NULL;
    }
}

err_t sys_mutex_new(sys_mutex_t *mutex) {
    sys_mutex_t created = calloc(1, sizeof(*created));
    if (!created) {
        return ERR_MEM;
    }

    spin_init(&created->lock);
    created->wait_head = NULL;
    created->wait_tail = NULL;
    created->owner = 0;
    created->depth = 0;
    created->locked = false;
    created->valid = true;
    *mutex = created;
    return ERR_OK;
}

void sys_spin_lock(sys_mutex_t *mutex) {
    sys_mutex_t m = NULL;
    wait_node_t wait_node;
    uintptr_t owner = naos_lwip_protect_owner_id();

    if (!mutex || !*mutex || !(*mutex)->valid) {
        return;
    }
    m = *mutex;

    if (!current_task || current_task->preempt_count) {
        for (;;) {
            bool acquired = false;

            spin_lock(&m->lock);
            if (!m->valid) {
                spin_unlock(&m->lock);
                return;
            }
            if (!m->locked) {
                m->locked = true;
                m->owner = owner;
                m->depth = 1;
                acquired = true;
            } else if (m->owner == owner) {
                m->depth++;
                acquired = true;
            }
            spin_unlock(&m->lock);

            if (acquired) {
                return;
            }
            arch_pause();
        }
    }

    memset(&wait_node, 0, sizeof(wait_node));
    wait_node.task = current_task;

    for (;;) {
        int reason = EOK;

        task_prepare_block(current_task);
        spin_lock(&m->lock);
        if (!m->valid) {
            naos_lwip_mutex_wait_remove_locked(m, &wait_node);
            spin_unlock(&m->lock);
            task_cancel_block_prepare(current_task);
            return;
        }
        if (!m->locked) {
            m->locked = true;
            m->owner = owner;
            m->depth = 1;
            naos_lwip_mutex_wait_remove_locked(m, &wait_node);
            spin_unlock(&m->lock);
            task_cancel_block_prepare(current_task);
            return;
        }
        if (m->owner == owner) {
            m->depth++;
            naos_lwip_mutex_wait_remove_locked(m, &wait_node);
            spin_unlock(&m->lock);
            task_cancel_block_prepare(current_task);
            return;
        }

        wait_node.task = current_task;
        naos_lwip_mutex_wait_enqueue_locked(m, &wait_node);
        spin_unlock(&m->lock);

        reason = task_block(current_task, TASK_BLOCKING, -1, "lwip_mutex_lock");
        if (reason < 0) {
            spin_lock(&m->lock);
            naos_lwip_mutex_wait_remove_locked(m, &wait_node);
            spin_unlock(&m->lock);
            task_cancel_block_prepare(current_task);
            return;
        }
    }
}

void sys_spin_unlock(sys_mutex_t *mutex) {
    sys_mutex_t m = NULL;
    uintptr_t owner = naos_lwip_protect_owner_id();
    wait_node_t *node = NULL;
    task_t *waiter = NULL;

    if (!mutex || !*mutex || !(*mutex)->valid) {
        return;
    }
    m = *mutex;

    spin_lock(&m->lock);
    if (!m->locked || m->owner != owner) {
        spin_unlock(&m->lock);
        return;
    }

    if (m->depth > 1) {
        m->depth--;
        spin_unlock(&m->lock);
        return;
    }

    while ((node = naos_lwip_mutex_wait_dequeue_locked(m))) {
        waiter = node->task;
        node->task = NULL;
        if (waiter && waiter->state != TASK_DIED)
            break;
        waiter = NULL;
    }

    m->locked = false;
    m->owner = 0;
    m->depth = 0;
    spin_unlock(&m->lock);

    if (waiter) {
        task_unblock(waiter, EOK);
        if (current_task && !current_task->preempt_count)
            sched_resched_if_needed();
    }
}

void sys_mutex_free(sys_mutex_t *mutex) {
    wait_node_t *node = NULL;
    bool has_waiters = false;

    if (!mutex || !*mutex) {
        return;
    }

    spin_lock(&(*mutex)->lock);
    has_waiters = (*mutex)->wait_head != NULL;
    (*mutex)->valid = false;
    (*mutex)->locked = false;
    (*mutex)->owner = 0;
    (*mutex)->depth = 0;
    node = naos_lwip_mutex_wait_dequeue_locked(*mutex);
    spin_unlock(&(*mutex)->lock);

    while (node) {
        task_t *waiter = node->task;
        node->task = NULL;
        if (waiter && waiter->state != TASK_DIED) {
            task_unblock(waiter, EOK);
        }

        spin_lock(&(*mutex)->lock);
        node = naos_lwip_mutex_wait_dequeue_locked(*mutex);
        spin_unlock(&(*mutex)->lock);
    }

    if (has_waiters) {
        *mutex = NULL;
        return;
    }

    free(*mutex);
    *mutex = NULL;
}

int sys_mutex_valid(sys_mutex_t *mutex) {
    return mutex && *mutex && (*mutex)->valid;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex) {
    if (mutex) {
        *mutex = NULL;
    }
}

static bool naos_lwip_mbox_op_get(sys_mbox_t *handle, sys_mbox_t *out) {
    bool acquired = false;
    sys_mbox_t mbox = NULL;

    if (!handle || !out)
        return false;

    spin_lock(&naos_lwip_mbox_lifecycle_lock);
    mbox = *handle;
    if (mbox && mbox->valid && !mbox->destroying) {
        mbox->active_ops++;
        *out = mbox;
        acquired = true;
    }
    spin_unlock(&naos_lwip_mbox_lifecycle_lock);
    return acquired;
}

static bool naos_lwip_mbox_op_put(sys_mbox_t mbox) {
    bool destroy = false;

    if (!mbox)
        return false;

    spin_lock(&naos_lwip_mbox_lifecycle_lock);
    if (mbox->active_ops)
        mbox->active_ops--;
    if (mbox->destroying && mbox->destroy_ready && !mbox->active_ops &&
        !mbox->destroy_claimed) {
        mbox->destroy_claimed = true;
        destroy = true;
    }
    spin_unlock(&naos_lwip_mbox_lifecycle_lock);
    return destroy;
}

static void naos_lwip_mbox_destroy(sys_mbox_t mbox) {
    if (!mbox)
        return;

    sys_mutex_free(&mbox->lock);
    free(mbox->not_empty);
    free(mbox->not_full);
    free(mbox->entries);
    free(mbox);
}

static void naos_lwip_mbox_sem_close(sys_sem_t sem) {
    wait_node_t *node = NULL;

    if (!sem)
        return;

    spin_lock(&sem->sem.lock);
    sem->valid = false;
    node = naos_lwip_sem_wait_dequeue_locked(sem);
    spin_unlock(&sem->sem.lock);

    while (node) {
        task_t *waiter = node->task;
        node->task = NULL;
        if (waiter && waiter->state != TASK_DIED)
            task_unblock(waiter, EOK);

        spin_lock(&sem->sem.lock);
        node = naos_lwip_sem_wait_dequeue_locked(sem);
        spin_unlock(&sem->sem.lock);
    }
}

err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    sys_mbox_t created = calloc(1, sizeof(*created));
    if (!created) {
        return ERR_MEM;
    }
    if (size <= 0) {
        size = 1;
    }

    created->entries = calloc((size_t)size, sizeof(void *));
    if (!created->entries) {
        free(created);
        return ERR_MEM;
    }

    created->size = (u32_t)size;
    created->valid = true;

    if (sys_sem_new(&created->not_empty, 0) != ERR_OK ||
        sys_sem_new(&created->not_full, (u8_t)MIN(size, 255)) != ERR_OK ||
        sys_mutex_new(&created->lock) != ERR_OK) {
        sys_sem_free(&created->not_empty);
        sys_sem_free(&created->not_full);
        sys_mutex_free(&created->lock);
        free(created->entries);
        free(created);
        return ERR_MEM;
    }

    for (int i = 255; i < size; i++) {
        sem_post(&created->not_full->sem);
    }

    *mbox = created;
    return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
    sys_mbox_t m = NULL;

    if (!naos_lwip_mbox_op_get(mbox, &m))
        return;

    while (sys_arch_sem_wait(&m->not_full, 0) == SYS_ARCH_TIMEOUT) {
        if (naos_lwip_mbox_op_put(m))
            naos_lwip_mbox_destroy(m);
        return;
    }

    sys_spin_lock(&m->lock);
    if (!m->valid || m->count >= m->size) {
        sys_spin_unlock(&m->lock);
        sys_sem_signal(&m->not_full);
        if (naos_lwip_mbox_op_put(m))
            naos_lwip_mbox_destroy(m);
        return;
    }
    m->entries[m->tail] = msg;
    m->tail = (m->tail + 1U) % m->size;
    m->count++;
    sys_spin_unlock(&m->lock);
    sys_sem_signal(&m->not_empty);
    if (naos_lwip_mbox_op_put(m))
        naos_lwip_mbox_destroy(m);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
    sys_mbox_t m = NULL;

    if (!naos_lwip_mbox_op_get(mbox, &m))
        return ERR_VAL;
    if (!naos_lwip_sem_trywait(m->not_full)) {
        if (naos_lwip_mbox_op_put(m))
            naos_lwip_mbox_destroy(m);
        return ERR_MEM;
    }

    sys_spin_lock(&m->lock);
    if (!m->valid || m->count >= m->size) {
        sys_spin_unlock(&m->lock);
        sys_sem_signal(&m->not_full);
        if (naos_lwip_mbox_op_put(m))
            naos_lwip_mbox_destroy(m);
        return ERR_VAL;
    }
    m->entries[m->tail] = msg;
    m->tail = (m->tail + 1U) % m->size;
    m->count++;
    sys_spin_unlock(&m->lock);
    sys_sem_signal(&m->not_empty);
    if (naos_lwip_mbox_op_put(m))
        naos_lwip_mbox_destroy(m);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg) {
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout) {
    u32_t waited = 0;
    sys_mbox_t m = NULL;

    if (!naos_lwip_mbox_op_get(mbox, &m)) {
        if (msg)
            *msg = NULL;
        return SYS_ARCH_TIMEOUT;
    }

    waited = sys_arch_sem_wait(&m->not_empty, timeout);
    if (waited == SYS_ARCH_TIMEOUT) {
        if (msg) {
            *msg = NULL;
        }
        if (naos_lwip_mbox_op_put(m))
            naos_lwip_mbox_destroy(m);
        return SYS_ARCH_TIMEOUT;
    }

    sys_spin_lock(&m->lock);
    if (!m->valid || m->count == 0) {
        sys_spin_unlock(&m->lock);
        if (msg) {
            *msg = NULL;
        }
        if (m->valid) {
            sys_sem_signal(&m->not_empty);
        }
        if (naos_lwip_mbox_op_put(m))
            naos_lwip_mbox_destroy(m);
        return SYS_ARCH_TIMEOUT;
    }
    if (msg) {
        *msg = m->entries[m->head];
    }
    m->entries[m->head] = NULL;
    m->head = (m->head + 1U) % m->size;
    m->count--;
    sys_spin_unlock(&m->lock);
    sys_sem_signal(&m->not_full);
    if (naos_lwip_mbox_op_put(m))
        naos_lwip_mbox_destroy(m);

    return waited;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
    sys_mbox_t m = NULL;

    if (!naos_lwip_mbox_op_get(mbox, &m)) {
        if (msg)
            *msg = NULL;
        return SYS_MBOX_EMPTY;
    }
    if (!naos_lwip_sem_trywait(m->not_empty)) {
        if (msg) {
            *msg = NULL;
        }
        if (naos_lwip_mbox_op_put(m))
            naos_lwip_mbox_destroy(m);
        return SYS_MBOX_EMPTY;
    }

    sys_spin_lock(&m->lock);
    if (!m->valid || m->count == 0) {
        sys_spin_unlock(&m->lock);
        if (msg) {
            *msg = NULL;
        }
        if (m->valid) {
            sys_sem_signal(&m->not_empty);
        }
        if (naos_lwip_mbox_op_put(m))
            naos_lwip_mbox_destroy(m);
        return SYS_MBOX_EMPTY;
    }
    if (msg) {
        *msg = m->entries[m->head];
    }
    m->entries[m->head] = NULL;
    m->head = (m->head + 1U) % m->size;
    m->count--;
    sys_spin_unlock(&m->lock);
    sys_sem_signal(&m->not_full);
    if (naos_lwip_mbox_op_put(m))
        naos_lwip_mbox_destroy(m);
    return 0;
}

void sys_mbox_free(sys_mbox_t *mbox) {
    sys_mbox_t m = NULL;
    bool destroy = false;

    if (!mbox) {
        return;
    }

    spin_lock(&naos_lwip_mbox_lifecycle_lock);
    m = *mbox;
    if (!m) {
        spin_unlock(&naos_lwip_mbox_lifecycle_lock);
        return;
    }
    *mbox = NULL;
    m->destroying = true;
    spin_unlock(&naos_lwip_mbox_lifecycle_lock);

    sys_spin_lock(&m->lock);
    m->valid = false;
    m->count = 0;
    sys_spin_unlock(&m->lock);

    naos_lwip_mbox_sem_close(m->not_empty);
    naos_lwip_mbox_sem_close(m->not_full);

    spin_lock(&naos_lwip_mbox_lifecycle_lock);
    m->destroy_ready = true;
    if (!m->active_ops && !m->destroy_claimed) {
        m->destroy_claimed = true;
        destroy = true;
    }
    spin_unlock(&naos_lwip_mbox_lifecycle_lock);
    if (destroy)
        naos_lwip_mbox_destroy(m);
}

int sys_mbox_valid(sys_mbox_t *mbox) {
    bool valid = false;

    if (!mbox)
        return false;

    spin_lock(&naos_lwip_mbox_lifecycle_lock);
    valid = *mbox && (*mbox)->valid && !(*mbox)->destroying;
    spin_unlock(&naos_lwip_mbox_lifecycle_lock);
    return valid;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox) {
    if (mbox) {
        spin_lock(&naos_lwip_mbox_lifecycle_lock);
        *mbox = NULL;
        spin_unlock(&naos_lwip_mbox_lifecycle_lock);
    }
}

static void lwip_sys_thread_entry(uint64_t arg) {
    lwip_thread_bootstrap_t *bootstrap = (lwip_thread_bootstrap_t *)arg;
    lwip_thread_fn fn = bootstrap->fn;
    void *thread_arg = bootstrap->arg;

    free(bootstrap);
    arch_enable_interrupt();
    fn(thread_arg);
    arch_disable_interrupt();
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg,
                            int stacksize, int prio) {
    LWIP_UNUSED_ARG(stacksize);
    LWIP_UNUSED_ARG(prio);

    lwip_thread_bootstrap_t *bootstrap = malloc(sizeof(*bootstrap));
    if (!bootstrap) {
        return NULL;
    }

    bootstrap->fn = thread;
    bootstrap->arg = arg;

    task_t *task = task_create(name ? name : "lwip", lwip_sys_thread_entry,
                               (uint64_t)bootstrap, NORMAL_PRIORITY);
    if (!task) {
        free(bootstrap);
        return NULL;
    }
    return task;
}

u32_t sys_now(void) { return (u32_t)(nano_time() / 1000000ULL); }

u32_t sys_jiffies(void) { return (u32_t)(nano_time() / 1000000ULL); }
