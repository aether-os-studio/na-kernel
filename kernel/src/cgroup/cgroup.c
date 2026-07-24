#include <cgroup/cgroup.h>
#include <fs/vfs/vfs.h>
#include <libs/string_builder.h>

typedef struct cgroup_assignment {
    uint64_t pid;
    cgroup_t *cgroup;
} cgroup_assignment_t;

struct cgroup_hierarchy {
    struct llist_header node;
    uint64_t id;
    bool unified;
    char *controllers;
    cgroup_t *root;
    /* PID lookups are on the task creation, exit and /proc hot paths. */
    hashmap_t assignments;
};

struct cgroup {
    struct llist_header sibling;
    struct llist_header children;
    struct cgroup *parent;
    char *name;
    uint32_t subtree_control;
    bool frozen;
    uint64_t runtime_ns;
    volatile int ref_count;
};

static spinlock_t cgroup_global_lock;
static cgroup_hierarchy_t *cgroup_unified_hierarchy;
static cgroup_t *cgroup_root_node;
static uint64_t cgroup_next_hierarchy_id = 1;
DEFINE_LLIST(cgroup_hierarchies);

static cgroup_t *
cgroup_task_cgroup_for_hierarchy_locked(uint64_t pid,
                                        cgroup_hierarchy_t *hierarchy);

void cgroup_lock(void) { spin_lock(&cgroup_global_lock); }

void cgroup_unlock(void) { spin_unlock(&cgroup_global_lock); }

void cgroup_account_runtime_ns(task_t *task, uint64_t delta_ns) {
    cgroup_hierarchy_t *hierarchy;
    cgroup_t *cgroup;

    if (!task || !delta_ns)
        return;

    cgroup_lock();
    hierarchy = cgroup_unified_hierarchy;
    cgroup = cgroup_task_cgroup_for_hierarchy_locked(task->pid, hierarchy);
    for (; cgroup; cgroup = cgroup_parent(cgroup))
        __atomic_add_fetch(&cgroup->runtime_ns, delta_ns, __ATOMIC_RELAXED);
    cgroup_unlock();
}

uint64_t cgroup_runtime_ns(cgroup_t *cgroup) {
    return cgroup ? __atomic_load_n(&cgroup->runtime_ns, __ATOMIC_RELAXED) : 0;
}

cgroup_t *cgroup_get(cgroup_t *cgroup) {
    if (!cgroup)
        return NULL;
    __atomic_add_fetch(&cgroup->ref_count, 1, __ATOMIC_ACQ_REL);
    return cgroup;
}

void cgroup_put(cgroup_t *cgroup) {
    if (!cgroup)
        return;
    if (__atomic_sub_fetch(&cgroup->ref_count, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    free(cgroup->name);
    free(cgroup);
}

cgroup_t *cgroup_root(void) { return cgroup_root_node; }

cgroup_hierarchy_t *cgroup_default_hierarchy(void) {
    return cgroup_unified_hierarchy;
}

cgroup_t *cgroup_hierarchy_root(cgroup_hierarchy_t *hierarchy) {
    return hierarchy ? hierarchy->root : cgroup_root_node;
}

static char *cgroup_normalize_controllers(const char *controllers,
                                          bool unified) {
    const char *input = controllers ? controllers : "";

    if (unified)
        return strdup("");
    if (strstr(input, "name=elogind"))
        return strdup("name=elogind");
    if (strstr(input, "name=systemd"))
        return strdup("name=systemd");
    if (strstr(input, "name=")) {
        const char *start = strstr(input, "name=");
        const char *end = start;
        size_t len;
        while (*end && *end != ',')
            end++;
        len = (size_t)(end - start);
        char *name = calloc(1, len + 1);
        if (!name)
            return NULL;
        memcpy(name, start, len);
        return name;
    }
    if (strstr(input, "none"))
        return strdup("");
    return strdup(input);
}

cgroup_hierarchy_t *cgroup_register_hierarchy(const char *controllers,
                                              bool unified) {
    cgroup_hierarchy_t *hierarchy, *tmp;
    char *normalized = cgroup_normalize_controllers(controllers, unified);

    if (!normalized)
        return NULL;

    cgroup_lock();
    llist_for_each(hierarchy, tmp, &cgroup_hierarchies, node) {
        if (hierarchy->unified == unified &&
            streq(hierarchy->controllers, normalized)) {
            free(normalized);
            cgroup_unlock();
            return hierarchy;
        }
    }

    hierarchy = calloc(1, sizeof(*hierarchy));
    if (!hierarchy) {
        free(normalized);
        cgroup_unlock();
        return NULL;
    }

    hierarchy->id = unified ? 0 : cgroup_next_hierarchy_id++;
    hierarchy->unified = unified;
    hierarchy->controllers = normalized;
    hierarchy->root = unified && cgroup_root_node ? cgroup_root_node
                                                  : cgroup_create(NULL, "");
    if (!hierarchy->root) {
        free(hierarchy->controllers);
        free(hierarchy);
        cgroup_unlock();
        return NULL;
    }
    if (hashmap_init(&hierarchy->assignments, 64) < 0) {
        if (hierarchy->root != cgroup_root_node)
            cgroup_put(hierarchy->root);
        free(hierarchy->controllers);
        free(hierarchy);
        cgroup_unlock();
        return NULL;
    }

    llist_init_head(&hierarchy->node);
    llist_append(&cgroup_hierarchies, &hierarchy->node);
    if (unified)
        cgroup_unified_hierarchy = hierarchy;
    cgroup_unlock();
    return hierarchy;
}

cgroup_t *cgroup_create(cgroup_t *parent, const char *name) {
    cgroup_t *cgroup = calloc(1, sizeof(*cgroup));

    if (!cgroup)
        return NULL;

    cgroup->parent = parent;
    cgroup->name = name ? strdup(name) : strdup("");
    if (!cgroup->name) {
        free(cgroup);
        return NULL;
    }

    llist_init_head(&cgroup->sibling);
    llist_init_head(&cgroup->children);
    cgroup->ref_count = 1;
    return cgroup;
}

const char *cgroup_name(cgroup_t *cgroup) {
    return cgroup && cgroup->name ? cgroup->name : "";
}

cgroup_t *cgroup_parent(cgroup_t *cgroup) {
    return cgroup ? cgroup->parent : NULL;
}

struct llist_header *cgroup_children(cgroup_t *cgroup) {
    return cgroup ? &cgroup->children : NULL;
}

struct llist_header *cgroup_sibling_node(cgroup_t *cgroup) {
    return cgroup ? &cgroup->sibling : NULL;
}

bool cgroup_is_descendant_of(cgroup_t *cgroup, cgroup_t *ancestor) {
    while (cgroup) {
        if (cgroup == ancestor)
            return true;
        cgroup = cgroup->parent;
    }
    return false;
}

size_t cgroup_descendant_count(cgroup_t *cgroup) {
    size_t count = 0;
    cgroup_t *child, *tmp;
    struct llist_header *children;

    if (!cgroup)
        return 0;

    children = cgroup_children(cgroup);
    llist_for_each(child, tmp, children, sibling) {
        count += 1 + cgroup_descendant_count(child);
    }
    return count;
}

uint32_t cgroup_subtree_control(cgroup_t *cgroup) {
    return cgroup ? cgroup->subtree_control : 0;
}

void cgroup_set_subtree_control(cgroup_t *cgroup, uint32_t mask) {
    if (cgroup)
        cgroup->subtree_control = mask;
}

bool cgroup_frozen(cgroup_t *cgroup) { return cgroup && cgroup->frozen; }

void cgroup_set_frozen(cgroup_t *cgroup, bool frozen) {
    if (cgroup)
        cgroup->frozen = frozen;
}

static cgroup_assignment_t *
cgroup_find_assignment_locked(uint64_t pid, cgroup_hierarchy_t *hierarchy) {
    return hierarchy ? (cgroup_assignment_t *)hashmap_get(
                           &hierarchy->assignments, pid)
                     : NULL;
}

static cgroup_t *
cgroup_task_cgroup_for_hierarchy_locked(uint64_t pid,
                                        cgroup_hierarchy_t *hierarchy) {
    cgroup_assignment_t *entry;

    if (!hierarchy)
        hierarchy = cgroup_unified_hierarchy;
    entry = cgroup_find_assignment_locked(pid, hierarchy);
    return entry ? entry->cgroup : cgroup_hierarchy_root(hierarchy);
}

cgroup_t *cgroup_task_cgroup_locked(uint64_t pid) {
    return cgroup_task_cgroup_for_hierarchy_locked(pid,
                                                   cgroup_unified_hierarchy);
}

cgroup_t *cgroup_task_cgroup(task_t *task) {
    cgroup_t *cgroup;

    cgroup_lock();
    cgroup = task ? cgroup_task_cgroup_locked(task->pid) : cgroup_root_node;
    cgroup = cgroup_get(cgroup);
    cgroup_unlock();
    return cgroup;
}

int cgroup_attach_task_pid_locked(uint64_t pid, cgroup_t *cgroup) {
    cgroup_hierarchy_t *hierarchy = cgroup_unified_hierarchy;
    cgroup_hierarchy_t *pos, *tmp;
    cgroup_assignment_t *entry;

    llist_for_each(pos, tmp, &cgroup_hierarchies, node) {
        if (cgroup_is_descendant_of(cgroup, pos->root)) {
            hierarchy = pos;
            break;
        }
    }

    entry = cgroup_find_assignment_locked(pid, hierarchy);

    if (!cgroup || cgroup == cgroup_hierarchy_root(hierarchy)) {
        if (!entry)
            return 0;
        hashmap_remove(&hierarchy->assignments, pid);
        free(entry);
        return 0;
    }

    if (entry) {
        entry->cgroup = cgroup;
        return 0;
    }

    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return -ENOMEM;

    entry->pid = pid;
    entry->cgroup = cgroup;
    if (hashmap_put(&hierarchy->assignments, pid, entry) < 0) {
        free(entry);
        return -ENOMEM;
    }
    return 0;
}

void cgroup_on_new_task(task_t *task) {
    cgroup_t *parent_cgroup;

    if (!task || !task_has_parent(task) || !task->parent)
        return;

    cgroup_lock();

    cgroup_hierarchy_t *hierarchy, *tmp;
    llist_for_each(hierarchy, tmp, &cgroup_hierarchies, node) {
        if (cgroup_find_assignment_locked(task->pid, hierarchy))
            continue;
        parent_cgroup = cgroup_task_cgroup_for_hierarchy_locked(
            task->parent->pid, hierarchy);
        if (parent_cgroup && parent_cgroup != cgroup_hierarchy_root(hierarchy))
            (void)cgroup_attach_task_pid_locked(task->pid, parent_cgroup);
    }

    cgroup_unlock();
}

void cgroup_on_exit_task(task_t *task) {
    if (!task)
        return;

    cgroup_lock();
    cgroup_hierarchy_t *hierarchy, *tmp;
    llist_for_each(hierarchy, tmp, &cgroup_hierarchies, node) {
        cgroup_assignment_t *entry =
            hashmap_remove(&hierarchy->assignments, task->pid);
        if (entry)
            free(entry);
    }
    cgroup_unlock();
}

static char *
cgroup_task_path_in_hierarchy_locked(task_t *task,
                                     cgroup_hierarchy_t *hierarchy) {
    char buf[VFS_PATH_MAX];
    size_t pos = sizeof(buf) - 1;
    cgroup_t *cgroup = NULL;

    buf[pos] = '\0';

    cgroup = task
                 ? cgroup_task_cgroup_for_hierarchy_locked(task->pid, hierarchy)
                 : cgroup_hierarchy_root(hierarchy);
    if (!cgroup || cgroup == cgroup_hierarchy_root(hierarchy)) {
        return strdup("/");
    }

    while (cgroup && cgroup != cgroup_hierarchy_root(hierarchy)) {
        size_t len = strlen(cgroup_name(cgroup));

        if (pos <= len + 1)
            break;
        pos -= len;
        memcpy(buf + pos, cgroup_name(cgroup), len);
        buf[--pos] = '/';
        cgroup = cgroup_parent(cgroup);
    }

    if (pos == sizeof(buf) - 1)
        buf[--pos] = '/';
    return strdup(buf + pos);
}

char *cgroup_task_path(task_t *task) {
    char *path;

    cgroup_lock();
    path = cgroup_task_path_in_hierarchy_locked(task, cgroup_unified_hierarchy);
    cgroup_unlock();
    return path;
}

char *cgroup_task_proc_text(task_t *task) {
    string_builder_t *builder = create_string_builder(128);
    cgroup_hierarchy_t *hierarchy, *tmp;

    if (!builder)
        return NULL;

    cgroup_lock();
    llist_for_each(hierarchy, tmp, &cgroup_hierarchies, node) {
        char *path;
        const char *cgroup_path;

        if (!hierarchy)
            continue;
        path = cgroup_task_path_in_hierarchy_locked(task, hierarchy);
        cgroup_path = path ? path : "/";
        if (hierarchy->unified) {
            string_builder_append(builder, "0::%s\n", cgroup_path);
        } else {
            string_builder_append(builder, "%llu:%s:%s\n",
                                  (unsigned long long)hierarchy->id,
                                  hierarchy->controllers, cgroup_path);
        }
        free(path);
    }
    cgroup_unlock();

    if (builder->size == 0)
        string_builder_append(builder, "0::/\n");

    char *data = builder->data;
    free(builder);
    return data;
}

void cgroup_init(void) {
    spin_init(&cgroup_global_lock);
    llist_init_head(&cgroup_hierarchies);
    cgroup_root_node = cgroup_create(NULL, "");
    ASSERT(cgroup_root_node);
    ASSERT(cgroup_register_hierarchy(NULL, true));
}
