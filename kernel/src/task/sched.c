#include "task/sched.h"
#include <irq/irq_manager.h>

extern sched_rq_t schedulers[MAX_CPU_NUM];

#define SCHED_NICE_MIN (-20)
#define SCHED_NICE_MAX 19
#define SCHED_NICE_0_LOAD 1024ULL
#define SCHED_WAKEUP_PREEMPT_GRANULARITY_NS 250000ULL
#define SCHED_LATENCY_NS 6000000ULL
#define SCHED_MIN_GRANULARITY_NS 750000ULL
#define SCHED_MAX_GRANULARITY_NS 4000000ULL

static const uint32_t sched_prio_to_weight[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
    1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
    110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static inline int sched_task_nice(task_t *task) {
    int nice = task ? task->nice : 0;

    if (nice < SCHED_NICE_MIN)
        return SCHED_NICE_MIN;
    if (nice > SCHED_NICE_MAX)
        return SCHED_NICE_MAX;
    return nice;
}

static inline uint64_t sched_task_weight(task_t *task) {
    return sched_prio_to_weight[sched_task_nice(task) - SCHED_NICE_MIN];
}

static inline uint64_t sched_nice_weight(int nice) {
    if (nice < SCHED_NICE_MIN)
        nice = SCHED_NICE_MIN;
    if (nice > SCHED_NICE_MAX)
        nice = SCHED_NICE_MAX;
    return sched_prio_to_weight[nice - SCHED_NICE_MIN];
}

static inline uint64_t sched_calc_delta_fair(uint64_t delta_ns, task_t *task) {
    uint64_t weight = sched_task_weight(task);

    if (!delta_ns || weight == SCHED_NICE_0_LOAD)
        return delta_ns;

    return (delta_ns * SCHED_NICE_0_LOAD) / weight;
}

static inline uint64_t sched_entity_slice_locked(sched_rq_t *scheduler,
                                                 struct sched_entity *entity) {
    uint64_t weight = sched_task_weight(entity ? entity->task : NULL);
    uint64_t total_weight = scheduler ? scheduler->load_weight : weight;
    uint64_t slice = SCHED_LATENCY_NS;

    if (scheduler && entity && entity->rq != scheduler)
        total_weight += weight;

    if (total_weight)
        slice = (SCHED_LATENCY_NS * weight) / total_weight;

    if (slice < SCHED_MIN_GRANULARITY_NS)
        return SCHED_MIN_GRANULARITY_NS;
    if (slice > SCHED_MAX_GRANULARITY_NS)
        return SCHED_MAX_GRANULARITY_NS;
    return slice;
}

static inline uint64_t sched_entity_deadline_locked(sched_rq_t *scheduler,
                                                    struct sched_entity *se) {
    uint64_t slice = sched_entity_slice_locked(scheduler, se);

    se->slice_ns = slice;
    return se->vruntime + sched_calc_delta_fair(slice, se->task);
}

static inline bool sched_entity_eligible_locked(uint64_t min_vruntime,
                                                struct sched_entity *se) {
    return se->vruntime <= min_vruntime;
}

static inline void sched_run_node_reset(rb_node_t *node) {
    if (node)
        memset(node, 0, sizeof(*node));
}

static inline uint64_t sched_subtree_min_vruntime(rb_node_t *node) {
    if (!node)
        return UINT64_MAX;

    return rb_entry(node, struct sched_entity, run_node)->subtree_min_vruntime;
}

static void sched_update_subtree_min_vruntime(rb_node_t *node) {
    struct sched_entity *entity = rb_entry(node, struct sched_entity, run_node);
    uint64_t min_vruntime = entity->vruntime;
    uint64_t child_min = sched_subtree_min_vruntime(node->rb_left);

    if (child_min < min_vruntime)
        min_vruntime = child_min;
    child_min = sched_subtree_min_vruntime(node->rb_right);
    if (child_min < min_vruntime)
        min_vruntime = child_min;
    entity->subtree_min_vruntime = min_vruntime;
}

static int sched_entity_cmp(struct sched_entity *left,
                            struct sched_entity *right) {
    if (left->deadline < right->deadline)
        return -1;
    if (left->deadline > right->deadline)
        return 1;
    if (left->vruntime < right->vruntime)
        return -1;
    if (left->vruntime > right->vruntime)
        return 1;

    uint64_t left_pid = left->task ? left->task->pid : 0;
    uint64_t right_pid = right->task ? right->task->pid : 0;
    if (left_pid < right_pid)
        return -1;
    if (left_pid > right_pid)
        return 1;

    return left < right ? -1 : (left > right ? 1 : 0);
}

static void sched_update_min_vruntime_locked(sched_rq_t *scheduler) {
    uint64_t min_vruntime = scheduler->min_vruntime;
    bool found = false;

    if (scheduler->curr && scheduler->curr != scheduler->idle &&
        scheduler->curr->task &&
        (scheduler->curr->task->state == TASK_READY ||
         scheduler->curr->task->current_state == TASK_RUNNING)) {
        min_vruntime = scheduler->curr->vruntime;
        found = true;
    }

    rb_node_t *root = scheduler->run_tree.rb_node;
    if (root) {
        uint64_t queued_min = sched_subtree_min_vruntime(root);

        if (!found || queued_min < min_vruntime)
            min_vruntime = queued_min;
        found = true;
    }

    if (found && min_vruntime > scheduler->min_vruntime)
        scheduler->min_vruntime = min_vruntime;
}

static bool sched_entity_preempts_curr_locked(sched_rq_t *scheduler,
                                              struct sched_entity *entity,
                                              uint64_t now_ns) {
    if (!scheduler || !entity || !entity->task ||
        entity->task->state != TASK_READY)
        return false;

    struct sched_entity *curr = scheduler->curr;
    if (!curr || !curr->task || curr == scheduler->idle)
        return true;
    if (curr == entity || curr->task == entity->task)
        return false;
    if (curr->task->state != TASK_READY ||
        curr->task->current_state != TASK_RUNNING)
        return true;

    uint64_t curr_vruntime = curr->vruntime;
    uint64_t curr_deadline = curr->deadline;
    if (curr->account_start_ns && now_ns > curr->account_start_ns) {
        uint64_t runtime =
            sched_calc_delta_fair(now_ns - curr->account_start_ns, curr->task);

        curr_vruntime += runtime;
        curr_deadline += runtime;
    }

    if (entity->vruntime + SCHED_WAKEUP_PREEMPT_GRANULARITY_NS < curr_vruntime)
        return true;
    if (entity->deadline + SCHED_WAKEUP_PREEMPT_GRANULARITY_NS < curr_deadline)
        return true;

    return false;
}

static void sched_entity_enqueue_locked(sched_rq_t *scheduler,
                                        struct sched_entity *entity) {
    rb_node_t **slot = &scheduler->run_tree.rb_node;
    rb_node_t *parent = NULL;

    entity->deadline = sched_entity_deadline_locked(scheduler, entity);
    entity->subtree_min_vruntime = entity->vruntime;

    while (*slot) {
        struct sched_entity *curr =
            rb_entry(*slot, struct sched_entity, run_node);
        parent = *slot;
        if (sched_entity_cmp(entity, curr) < 0)
            slot = &(*slot)->rb_left;
        else
            slot = &(*slot)->rb_right;
    }

    sched_run_node_reset(&entity->run_node);
    rb_set_parent(&entity->run_node, parent);
    rb_set_color(&entity->run_node, KRB_RED);
    *slot = &entity->run_node;
    rb_insert_augmented(&entity->run_node, &scheduler->run_tree,
                        sched_update_subtree_min_vruntime);

    entity->on_rq = true;
    entity->rq = scheduler;
    if (!entity->task || entity->task->state != TASK_READY)
        scheduler->cleanup_queued = true;
    scheduler->nr_queued++;
    scheduler->nr_running++;
    __atomic_store_n(&scheduler->nr_running_snapshot, scheduler->nr_running,
                     __ATOMIC_RELAXED);
    scheduler->load_weight += sched_task_weight(entity->task);
}

static void sched_entity_dequeue_locked(sched_rq_t *scheduler,
                                        struct sched_entity *entity,
                                        bool update_min_vruntime) {
    rb_erase_augmented(&entity->run_node, &scheduler->run_tree,
                       sched_update_subtree_min_vruntime);
    sched_run_node_reset(&entity->run_node);

    entity->on_rq = false;
    entity->rq = NULL;
    if (scheduler->nr_queued)
        scheduler->nr_queued--;
    if (scheduler->nr_running)
        scheduler->nr_running--;
    __atomic_store_n(&scheduler->nr_running_snapshot, scheduler->nr_running,
                     __ATOMIC_RELAXED);

    uint64_t weight = sched_task_weight(entity->task);
    scheduler->load_weight =
        scheduler->load_weight > weight ? scheduler->load_weight - weight : 0;
    if (update_min_vruntime)
        sched_update_min_vruntime_locked(scheduler);
}

static void sched_entity_reweight_current_locked(sched_rq_t *scheduler,
                                                 struct sched_entity *entity,
                                                 uint64_t old_weight,
                                                 int new_nice,
                                                 uint64_t now_ns) {
    if (!scheduler || !entity)
        return;

    task_t *task = entity->task;
    if (!task)
        return;

    if (entity->account_start_ns && now_ns > entity->account_start_ns) {
        uint64_t delta_ns = now_ns - entity->account_start_ns;

        entity->account_start_ns = now_ns;
        if (old_weight == SCHED_NICE_0_LOAD)
            entity->vruntime += delta_ns;
        else
            entity->vruntime += (delta_ns * SCHED_NICE_0_LOAD) / old_weight;
    }

    task->nice = new_nice;
    uint64_t new_weight = sched_task_weight(entity->task);

    scheduler->load_weight = scheduler->load_weight > old_weight
                                 ? scheduler->load_weight - old_weight
                                 : 0;
    scheduler->load_weight += new_weight;

    entity->slice_ns = sched_entity_slice_locked(scheduler, entity);
    entity->deadline = sched_entity_deadline_locked(scheduler, entity);
}

static void sched_curr_attach_locked(sched_rq_t *scheduler,
                                     struct sched_entity *entity,
                                     uint64_t now_ns) {
    scheduler->curr = entity;
    entity->rq = scheduler;
    entity->on_rq = false;
    scheduler->nr_running++;
    __atomic_store_n(&scheduler->nr_running_snapshot, scheduler->nr_running,
                     __ATOMIC_RELAXED);
    scheduler->load_weight += sched_task_weight(entity->task);
    entity->exec_start_ns = now_ns;
    entity->account_start_ns = now_ns;
    entity->slice_ns = sched_entity_slice_locked(scheduler, entity);
    entity->deadline = sched_entity_deadline_locked(scheduler, entity);
}

static void sched_curr_detach_locked(sched_rq_t *scheduler,
                                     struct sched_entity *entity) {
    if (scheduler->curr == entity)
        scheduler->curr = scheduler->idle;

    if (scheduler->nr_running)
        scheduler->nr_running--;
    __atomic_store_n(&scheduler->nr_running_snapshot, scheduler->nr_running,
                     __ATOMIC_RELAXED);

    uint64_t weight = sched_task_weight(entity->task);
    scheduler->load_weight =
        scheduler->load_weight > weight ? scheduler->load_weight - weight : 0;
    entity->rq = NULL;
    entity->on_rq = false;
    sched_update_min_vruntime_locked(scheduler);
}

static bool sched_entity_is_current_locked(sched_rq_t *scheduler,
                                           struct sched_entity *entity) {
    return scheduler && entity && scheduler->curr == entity && !entity->on_rq &&
           entity->rq == scheduler;
}

static void sched_add_entity(task_t *task, sched_rq_t *scheduler, bool wakeup) {
    if (__builtin_expect(!task || !scheduler || !task->sched_info, 0))
        return;

    struct sched_entity *entity = task->sched_info;
    bool should_ping_cpu = false;
    task_t *resched_task = NULL;
    uint32_t target_cpu = task->cpu_id;
    uint64_t now_ns = nano_time();

    /*
     * Scheduler entry points are also called from interruptible syscall
     * context (clone, wakeups and affinity/nice changes).  Keep local IRQs
     * disabled while a runqueue lock is held, otherwise a timer interrupt can
     * re-enter the scheduler on this CPU and spin forever on its own lock.
     */
    spin_lock(&scheduler->lock);

    entity->task = task;

    if (scheduler->idle == entity) {
        spin_unlock(&scheduler->lock);
        return;
    }

    if (entity->on_rq || sched_entity_is_current_locked(scheduler, entity)) {
        spin_unlock(&scheduler->lock);
        return;
    }

    if (entity->rq && entity->rq != scheduler) {
        spin_unlock(&scheduler->lock);
        return;
    }

    uint64_t placement = scheduler->min_vruntime;
    if (wakeup && placement > SCHED_WAKEUP_PREEMPT_GRANULARITY_NS)
        placement -= SCHED_WAKEUP_PREEMPT_GRANULARITY_NS;
    else if (wakeup)
        placement = 0;
    if (entity->vruntime < placement)
        entity->vruntime = placement;

    sched_entity_enqueue_locked(scheduler, entity);
    sched_update_min_vruntime_locked(scheduler);

    if (scheduler->curr && scheduler->curr->task &&
        scheduler->curr->task != task &&
        sched_entity_preempts_curr_locked(scheduler, entity, now_ns))
        resched_task = scheduler->curr->task;
    should_ping_cpu =
        target_cpu < cpu_count && target_cpu != current_cpu_id &&
        sched_entity_preempts_curr_locked(scheduler, entity, now_ns);

    spin_unlock(&scheduler->lock);

    if (resched_task)
        task_set_need_resched_once(resched_task);

    if (should_ping_cpu)
        irq_trigger_sched_ipi(target_cpu);
}

void add_sched_entity(task_t *task, sched_rq_t *scheduler) {
    sched_add_entity(task, scheduler, false);
}

void add_sched_entity_wakeup(task_t *task, sched_rq_t *scheduler) {
    sched_add_entity(task, scheduler, true);
}

void remove_sched_entity(task_t *task, sched_rq_t *scheduler) {
    if (__builtin_expect(!task || !scheduler || !task->sched_info, 0))
        return;

    struct sched_entity *entity = task->sched_info;

    spin_lock(&scheduler->lock);

    if (entity->on_rq && entity->rq == scheduler) {
        sched_entity_dequeue_locked(scheduler, entity, true);
    } else if (sched_entity_is_current_locked(scheduler, entity)) {
        sched_curr_detach_locked(scheduler, entity);
    }

    spin_unlock(&scheduler->lock);
}

void sched_account_runtime_until(task_t *task, uint64_t now_ns) {
    if (!task || !task->sched_info)
        return;

    struct sched_entity *entity = task->sched_info;
    sched_rq_t *scheduler = entity->rq;

    if (!scheduler && task->cpu_id < MAX_CPU_NUM)
        scheduler = &schedulers[task->cpu_id];
    if (!scheduler)
        return;

    spin_lock(&scheduler->lock);

    bool was_queued = entity->on_rq && entity->rq == scheduler;
    if (was_queued)
        sched_entity_dequeue_locked(scheduler, entity, true);

    uint64_t delta_ns =
        entity->account_start_ns && now_ns > entity->account_start_ns
            ? now_ns - entity->account_start_ns
            : 0;
    entity->vruntime += sched_calc_delta_fair(delta_ns, task);

    if (sched_entity_is_current_locked(scheduler, entity)) {
        entity->account_start_ns = now_ns;
        entity->slice_ns = sched_entity_slice_locked(scheduler, entity);
        entity->deadline = sched_entity_deadline_locked(scheduler, entity);
    }

    if (was_queued)
        sched_entity_enqueue_locked(scheduler, entity);

    sched_update_min_vruntime_locked(scheduler);

    spin_unlock(&scheduler->lock);
}

void sched_set_task_nice(task_t *task, int niceval) {
    if (!task)
        return;

    if (niceval < SCHED_NICE_MIN)
        niceval = SCHED_NICE_MIN;
    if (niceval > SCHED_NICE_MAX)
        niceval = SCHED_NICE_MAX;

    if (task->nice == niceval)
        return;

    struct sched_entity *entity = task->sched_info;
    uint64_t now_ns = nano_time();

    if (!entity) {
        task->nice = niceval;
        return;
    }

    sched_rq_t *scheduler = entity->rq;
    if (!scheduler && task->cpu_id < MAX_CPU_NUM)
        scheduler = &schedulers[task->cpu_id];
    if (!scheduler) {
        task->nice = niceval;
        return;
    }

    spin_lock(&scheduler->lock);

    if (entity->on_rq && entity->rq == scheduler) {
        sched_entity_dequeue_locked(scheduler, entity, true);
        task->nice = niceval;
        sched_entity_enqueue_locked(scheduler, entity);
    } else if (sched_entity_is_current_locked(scheduler, entity)) {
        uint64_t old_weight = sched_nice_weight(task->nice);
        sched_entity_reweight_current_locked(scheduler, entity, old_weight,
                                             niceval, now_ns);
    } else {
        task->nice = niceval;
    }

    sched_update_min_vruntime_locked(scheduler);

    spin_unlock(&scheduler->lock);
}

static struct sched_entity *sched_first_eligible_locked(sched_rq_t *scheduler,
                                                        rb_node_t *node,
                                                        uint64_t max_vruntime,
                                                        task_t *excluded) {
    if (!node || sched_subtree_min_vruntime(node) > max_vruntime)
        return NULL;

    struct sched_entity *eligible = sched_first_eligible_locked(
        scheduler, node->rb_left, max_vruntime, excluded);
    if (eligible)
        return eligible;

    struct sched_entity *entity = rb_entry(node, struct sched_entity, run_node);
    if (sched_entity_eligible_locked(max_vruntime, entity)) {
        task_t *candidate = entity->task;

        if (!candidate || candidate->state != TASK_READY)
            scheduler->cleanup_queued = true;
        else if (candidate != excluded)
            return entity;
    }

    return sched_first_eligible_locked(scheduler, node->rb_right, max_vruntime,
                                       excluded);
}

static struct sched_entity *
sched_pick_min_fallback_locked(sched_rq_t *scheduler, task_t *excluded) {
    struct sched_entity *best = NULL;

    for (rb_node_t *node = rb_first(&scheduler->run_tree); node;
         node = rb_next(node)) {
        struct sched_entity *entity =
            rb_entry(node, struct sched_entity, run_node);
        task_t *candidate = entity->task;

        if (!candidate || candidate->state != TASK_READY) {
            scheduler->cleanup_queued = true;
            continue;
        }
        if (candidate == excluded)
            continue;
        if (!best || entity->vruntime < best->vruntime)
            best = entity;
    }

    return best;
}

static struct sched_entity *sched_pick_eevdf_locked(sched_rq_t *scheduler,
                                                    task_t *excluded) {
    rb_node_t *root = scheduler->run_tree.rb_node;
    if (!root)
        return NULL;

    uint64_t effective_min_vruntime = scheduler->min_vruntime;
    uint64_t queued_min = sched_subtree_min_vruntime(root);
    if (queued_min > effective_min_vruntime)
        effective_min_vruntime = queued_min;

    struct sched_entity *eligible = sched_first_eligible_locked(
        scheduler, root, effective_min_vruntime, excluded);
    if (eligible)
        return eligible;

    /*
     * The augmented minimum is an optimisation, not a correctness condition.
     * In particular, rotations/deletions can leave a transiently conservative
     * subtree minimum.  Never report an empty runqueue while a READY entity is
     * still present: doing so sends the CPU to idle and strands that task until
     * an unrelated interrupt happens to run the scheduler again.
     */
    return sched_pick_min_fallback_locked(scheduler, excluded);
}

static void sched_drop_dead_queued_locked(sched_rq_t *scheduler) {
    rb_node_t *node = rb_first(&scheduler->run_tree);
    bool dropped = false;

    while (node) {
        struct sched_entity *se = rb_entry(node, struct sched_entity, run_node);
        rb_node_t *next = rb_next(node);
        task_t *task = se ? se->task : NULL;

        if (!task || task->state != TASK_READY) {
            dropped = true;
            sched_entity_dequeue_locked(scheduler, se, false);
            node = next;
            continue;
        }

        node = next;
    }

    if (dropped) {
        sched_update_min_vruntime_locked(scheduler);
    }
    scheduler->cleanup_queued = false;
}

bool sched_should_preempt(sched_rq_t *scheduler, task_t *curr_task,
                          uint64_t now_ns) {
    bool should_preempt = false;

    if (!scheduler || !curr_task || !curr_task->sched_info)
        return true;

    struct sched_entity *curr = curr_task->sched_info;

    spin_lock(&scheduler->lock);

    if (scheduler->idle && curr_task == scheduler->idle->task) {
        should_preempt = scheduler->nr_queued != 0;
        goto out;
    }

    if (curr_task->state != TASK_READY ||
        curr_task->current_state != TASK_RUNNING) {
        should_preempt = true;
        goto out;
    }

    if (!scheduler->nr_queued)
        goto out;

    if (!sched_entity_is_current_locked(scheduler, curr)) {
        should_preempt = true;
        goto out;
    }

    if (!curr->exec_start_ns || now_ns < curr->exec_start_ns)
        curr->exec_start_ns = now_ns;

    if (scheduler->cleanup_queued)
        sched_drop_dead_queued_locked(scheduler);

    uint64_t ran_ns = now_ns - curr->exec_start_ns;
    uint64_t slice_ns = curr->slice_ns
                            ? curr->slice_ns
                            : sched_entity_slice_locked(scheduler, curr);
    struct sched_entity *next = sched_pick_eevdf_locked(scheduler, curr_task);
    if (scheduler->cleanup_queued) {
        sched_drop_dead_queued_locked(scheduler);
        next = sched_pick_eevdf_locked(scheduler, curr_task);
    }
    if (!next)
        goto out;

    if (next->deadline + SCHED_WAKEUP_PREEMPT_GRANULARITY_NS < curr->deadline) {
        should_preempt = true;
        goto out;
    }

    if (ran_ns >= slice_ns)
        should_preempt = true;

out:
    spin_unlock(&scheduler->lock);
    return should_preempt;
}

uint64_t sched_next_preempt_deadline(sched_rq_t *scheduler, task_t *curr_task,
                                     uint64_t now_ns) {
    uint64_t deadline = UINT64_MAX;

    if (!scheduler || !curr_task || !curr_task->sched_info)
        return now_ns;

    struct sched_entity *curr = curr_task->sched_info;

    spin_lock(&scheduler->lock);

    if (scheduler->idle && curr == scheduler->idle)
        goto out;
    if (!sched_entity_is_current_locked(scheduler, curr))
        goto out_now;
    if (curr_task->state != TASK_READY ||
        curr_task->current_state != TASK_RUNNING)
        goto out_now;
    if (!scheduler->nr_queued)
        goto out;

    uint64_t slice_ns = curr->slice_ns
                            ? curr->slice_ns
                            : sched_entity_slice_locked(scheduler, curr);
    uint64_t start_ns = curr->exec_start_ns ? curr->exec_start_ns : now_ns;

    if (now_ns >= start_ns + slice_ns)
        goto out_now;
    deadline = start_ns + slice_ns;
    goto out;

out_now:
    deadline = now_ns;
out:
    spin_unlock(&scheduler->lock);
    return deadline;
}

static task_t *sched_pick_next_task_internal(sched_rq_t *scheduler,
                                             task_t *excluded,
                                             task_t *requeue_task, bool yielded,
                                             bool fallback_to_excluded) {
    task_t *next_task = NULL;
    struct sched_entity *deferred_enqueue = NULL;
    uint64_t now_ns = nano_time();

    spin_lock(&scheduler->lock);

    if (scheduler->cleanup_queued)
        sched_drop_dead_queued_locked(scheduler);

    if (requeue_task && requeue_task->sched_info) {
        struct sched_entity *entity = requeue_task->sched_info;

        if (scheduler->idle != entity &&
            sched_entity_is_current_locked(scheduler, entity)) {
            if (yielded && !scheduler->nr_queued) {
                next_task = requeue_task;
                goto out;
            }

            sched_curr_detach_locked(scheduler, entity);
            if (entity->vruntime < scheduler->min_vruntime)
                entity->vruntime = scheduler->min_vruntime;
            if (yielded && scheduler->nr_queued &&
                entity->vruntime != UINT64_MAX)
                entity->vruntime++;
            if (yielded)
                deferred_enqueue = entity;
            else
                sched_entity_enqueue_locked(scheduler, entity);
        }
    }

    struct sched_entity *next = sched_pick_eevdf_locked(scheduler, excluded);
    if (scheduler->cleanup_queued) {
        sched_drop_dead_queued_locked(scheduler);
        next = sched_pick_eevdf_locked(scheduler, excluded);
    }
    if (!next && fallback_to_excluded)
        next = sched_pick_eevdf_locked(scheduler, NULL);

    if (next) {
        if (scheduler->curr == scheduler->idle && scheduler->idle) {
            scheduler->idle->rq = NULL;
            scheduler->idle->on_rq = false;
        }
        sched_entity_dequeue_locked(scheduler, next, true);
        sched_curr_attach_locked(scheduler, next, now_ns);
        if (deferred_enqueue)
            sched_entity_enqueue_locked(scheduler, deferred_enqueue);
        next_task = next->task;
        goto out;
    }

    if (deferred_enqueue) {
        sched_curr_attach_locked(scheduler, deferred_enqueue, now_ns);
        next_task = deferred_enqueue->task;
        goto out;
    }

    if (scheduler->idle) {
        scheduler->curr = scheduler->idle;
        scheduler->idle->rq = scheduler;
        scheduler->idle->on_rq = false;
        scheduler->idle->exec_start_ns = now_ns;
        scheduler->idle->slice_ns = 0;
        scheduler->idle->deadline = scheduler->min_vruntime;
        next_task = scheduler->idle->task;
    }

out:
    sched_update_min_vruntime_locked(scheduler);
    spin_unlock(&scheduler->lock);
    return next_task;
}

task_t *sched_requeue_current_and_pick_next(task_t *task, sched_rq_t *scheduler,
                                            bool yielded) {
    return sched_pick_next_task_internal(scheduler, NULL, task, yielded, false);
}

task_t *sched_pick_next_task(sched_rq_t *scheduler) {
    return sched_pick_next_task_internal(scheduler, NULL, NULL, false, false);
}

task_t *sched_pick_next_task_excluding(sched_rq_t *scheduler,
                                       task_t *excluded) {
    return sched_pick_next_task_internal(scheduler, excluded, NULL, false,
                                         false);
}

size_t sched_rq_nr_running(sched_rq_t *scheduler) {
    size_t nr_running = 0;

    if (!scheduler)
        return 0;

    spin_lock(&scheduler->lock);
    nr_running = scheduler->nr_running;
    spin_unlock(&scheduler->lock);
    return nr_running;
}

size_t sched_rq_nr_queued(sched_rq_t *scheduler) {
    size_t nr_queued = 0;

    if (!scheduler)
        return 0;

    spin_lock(&scheduler->lock);
    nr_queued = scheduler->nr_queued;
    spin_unlock(&scheduler->lock);
    return nr_queued;
}

size_t sched_rq_nr_running_snapshot(sched_rq_t *scheduler) {
    if (!scheduler)
        return 0;

    return __atomic_load_n(&scheduler->nr_running_snapshot, __ATOMIC_RELAXED);
}
