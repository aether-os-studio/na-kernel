#pragma once

#include <libs/klibc.h>
#include <libs/llist.h>
#include <task/task.h>

typedef struct cgroup cgroup_t;
typedef struct cgroup_hierarchy cgroup_hierarchy_t;

void cgroup_init(void);
cgroup_hierarchy_t *cgroup_register_hierarchy(const char *controllers,
                                              bool unified);
cgroup_hierarchy_t *cgroup_default_hierarchy(void);
cgroup_t *cgroup_root(void);
cgroup_t *cgroup_hierarchy_root(cgroup_hierarchy_t *hierarchy);
cgroup_t *cgroup_create(cgroup_t *parent, const char *name);
cgroup_t *cgroup_get(cgroup_t *cgroup);
void cgroup_put(cgroup_t *cgroup);

const char *cgroup_name(cgroup_t *cgroup);
cgroup_t *cgroup_parent(cgroup_t *cgroup);
struct llist_header *cgroup_children(cgroup_t *cgroup);
struct llist_header *cgroup_sibling_node(cgroup_t *cgroup);
bool cgroup_is_descendant_of(cgroup_t *cgroup, cgroup_t *ancestor);
size_t cgroup_descendant_count(cgroup_t *cgroup);

uint32_t cgroup_subtree_control(cgroup_t *cgroup);
void cgroup_set_subtree_control(cgroup_t *cgroup, uint32_t mask);
bool cgroup_frozen(cgroup_t *cgroup);
void cgroup_set_frozen(cgroup_t *cgroup, bool frozen);
uint64_t cgroup_runtime_ns(cgroup_t *cgroup);

void cgroup_lock(void);
void cgroup_unlock(void);
int cgroup_attach_task_pid_locked(uint64_t pid, cgroup_t *cgroup);
cgroup_t *cgroup_task_cgroup_locked(uint64_t pid);
cgroup_t *cgroup_task_cgroup(task_t *task);
void cgroup_on_new_task(task_t *task);
void cgroup_on_exit_task(task_t *task);
char *cgroup_task_path(task_t *task);
char *cgroup_task_proc_text(task_t *task);
