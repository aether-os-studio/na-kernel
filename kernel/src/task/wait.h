#pragma once

#include <libs/klibc.h>
#include <libs/llist.h>

struct task;
typedef struct task task_t;

typedef struct wait_queue_entry wait_queue_entry_t;

typedef int (*wait_queue_wake_func_t)(wait_queue_entry_t *entry,
                                      uint32_t events, int reason);

typedef struct wait_queue_head {
    spinlock_t lock;
    struct llist_header entries;
    uint64_t wake_seq;
} wait_queue_head_t;

typedef struct wait_mutex {
    wait_queue_head_t wait;
    bool locked;
} wait_mutex_t;

struct wait_queue_entry {
    struct llist_header node;
    task_t *task;
    uint32_t events;
    wait_queue_wake_func_t wake;
    void *private_data;
    uint64_t wake_seq;
};

void wait_queue_init(wait_queue_head_t *queue);
void wait_queue_entry_init(wait_queue_entry_t *entry, task_t *task,
                           uint32_t events, wait_queue_wake_func_t wake,
                           void *private_data);
void wait_queue_add(wait_queue_head_t *queue, wait_queue_entry_t *entry);
void wait_queue_remove(wait_queue_head_t *queue, wait_queue_entry_t *entry);
int wait_queue_wake_entry(wait_queue_entry_t *entry, uint32_t events,
                          int reason);
int wait_queue_wake(wait_queue_head_t *queue, uint32_t events, int nr,
                    int reason);
int wait_queue_wake_all(wait_queue_head_t *queue, uint32_t events, int reason);

void wait_mutex_init(wait_mutex_t *mutex);
bool wait_mutex_trylock(wait_mutex_t *mutex);
void wait_mutex_lock(wait_mutex_t *mutex);
void wait_mutex_unlock(wait_mutex_t *mutex);
