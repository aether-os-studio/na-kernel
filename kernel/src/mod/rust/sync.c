#include <libs/klibc.h>
#include <mod/rust/api.h>
#include <task/wait.h>

_Static_assert(sizeof(RawSpinLock) == sizeof(spinlock_t),
               "spinlock size must match the Rust ABI");
_Static_assert(offsetof(RawSpinLock, lock) == offsetof(spinlock_t, lock),
               "spinlock lock offset must match the Rust ABI");
_Static_assert(offsetof(RawSpinLock, irq_state) ==
                   offsetof(spinlock_t, irq_state),
               "spinlock irq state offset must match the Rust ABI");

struct na_mutex {
    wait_mutex_t lock;
};

void na_spin_lock(RawSpinLock *lock) { spin_lock((spinlock_t *)lock); }

void na_spin_unlock(RawSpinLock *lock) { spin_unlock((spinlock_t *)lock); }

na_mutex_t *na_mutex_create(void) {
    na_mutex_t *mutex = calloc(1, sizeof(*mutex));
    if (mutex)
        wait_mutex_init(&mutex->lock);
    return mutex;
}

void na_mutex_destroy(na_mutex_t *mutex) { free(mutex); }

void na_mutex_lock(na_mutex_t *mutex) {
    if (mutex)
        wait_mutex_lock(&mutex->lock);
}

void na_mutex_unlock(na_mutex_t *mutex) {
    if (mutex)
        wait_mutex_unlock(&mutex->lock);
}
