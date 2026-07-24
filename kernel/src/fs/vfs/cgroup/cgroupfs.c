#include <fs/vfs/cgroup/cgroupfs.h>
#include <cgroup/cgroup.h>
#include <fs/fs_syscall.h>
#include <fs/vfs/notify.h>
#include <fs/vfs/vfs.h>
#include <libs/string_builder.h>
#include <task/task.h>

typedef enum cgroupfs_controller_mask {
    CGROUPFS_CTRL_CPU = 1U << 0,
    CGROUPFS_CTRL_IO = 1U << 1,
    CGROUPFS_CTRL_MEMORY = 1U << 2,
    CGROUPFS_CTRL_PIDS = 1U << 3,
} cgroupfs_controller_mask_t;

#define CGROUPFS_ALL_CONTROLLERS                                               \
    (CGROUPFS_CTRL_CPU | CGROUPFS_CTRL_IO | CGROUPFS_CTRL_MEMORY |             \
     CGROUPFS_CTRL_PIDS)

typedef enum cgroupfs_inode_kind {
    CGROUPFS_INODE_DIR = 0,
    CGROUPFS_INODE_CGROUP_PROCS,
    CGROUPFS_INODE_CGROUP_THREADS,
    CGROUPFS_INODE_CGROUP_CONTROLLERS,
    CGROUPFS_INODE_CGROUP_EVENTS,
    CGROUPFS_INODE_CGROUP_TYPE,
    CGROUPFS_INODE_CGROUP_FREEZE,
    CGROUPFS_INODE_CGROUP_SUBTREE_CONTROL,
    CGROUPFS_INODE_CGROUP_MAX_DEPTH,
    CGROUPFS_INODE_CGROUP_MAX_DESCENDANTS,
    CGROUPFS_INODE_CGROUP_STAT,
    CGROUPFS_INODE_CPU_STAT,
} cgroupfs_inode_kind_t;

typedef struct cgroupfs_dirent {
    struct llist_header node;
    char *name;
    struct vfs_inode *inode;
} cgroupfs_dirent_t;

typedef struct cgroupfs_fs_info {
    cgroup_hierarchy_t *hierarchy;
    bool unified;
} cgroupfs_fs_info_t;

typedef struct cgroupfs_inode_info {
    struct vfs_inode vfs_inode;
    struct llist_header children;
    struct llist_header events_node;
    cgroup_t *cgroup;
    cgroupfs_inode_kind_t kind;
} cgroupfs_inode_info_t;

static struct vfs_file_system_type cgroupfs_fs_type;
static struct vfs_file_system_type cgroupfs_legacy_fs_type;
static const struct vfs_super_operations cgroupfs_super_ops;
static const struct vfs_inode_operations cgroupfs_inode_ops;
static const struct vfs_file_operations cgroupfs_dir_file_ops;
static const struct vfs_file_operations cgroupfs_file_ops;
static spinlock_t cgroupfs_events_lock = SPIN_INIT;
static DEFINE_LLIST(cgroupfs_events_inodes);

static inline cgroupfs_inode_info_t *cgroupfs_i(struct vfs_inode *inode) {
    return inode ? container_of(inode, cgroupfs_inode_info_t, vfs_inode) : NULL;
}

static inline cgroupfs_fs_info_t *cgroupfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (cgroupfs_fs_info_t *)sb->s_fs_info : NULL;
}

static struct vfs_inode *cgroupfs_alloc_inode(struct vfs_super_block *sb) {
    cgroupfs_inode_info_t *info = calloc(1, sizeof(*info));
    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void cgroupfs_destroy_inode(struct vfs_inode *inode) {
    free(cgroupfs_i(inode));
}

static void cgroupfs_evict_inode(struct vfs_inode *inode) {
    cgroupfs_inode_info_t *info = cgroupfs_i(inode);
    cgroupfs_dirent_t *de, *tmp;

    if (!info)
        return;

    if (info->kind == CGROUPFS_INODE_CGROUP_EVENTS) {
        spin_lock(&cgroupfs_events_lock);
        if (!llist_empty(&info->events_node))
            llist_delete(&info->events_node);
        spin_unlock(&cgroupfs_events_lock);
    }

    if (info->kind == CGROUPFS_INODE_DIR) {
        llist_for_each(de, tmp, &info->children, node) {
            llist_delete(&de->node);
            if (de->inode)
                vfs_iput(de->inode);
            free(de->name);
            free(de);
        }
    }

    if (info->cgroup) {
        cgroup_put(info->cgroup);
        info->cgroup = NULL;
    }
}

static int cgroupfs_statfs(struct vfs_path *path, void *buf) {
    struct statfs *st = (struct statfs *)buf;

    (void)path;
    if (!st)
        return -EINVAL;

    memset(st, 0, sizeof(*st));
    st->f_type = 0x63677270ULL;
    st->f_bsize = PAGE_SIZE;
    st->f_frsize = PAGE_SIZE;
    st->f_namelen = 255;
    return 0;
}

static struct vfs_inode *
cgroupfs_new_inode(struct vfs_super_block *sb, struct vfs_inode *dir,
                   cgroup_t *cgroup, cgroupfs_inode_kind_t kind, umode_t mode) {
    struct vfs_inode *inode = vfs_alloc_inode(sb);
    cgroupfs_inode_info_t *info = cgroupfs_i(inode);

    if (!inode)
        return NULL;

    llist_init_head(&info->children);
    llist_init_head(&info->events_node);
    info->kind = kind;
    info->cgroup = cgroup_get(cgroup);

    inode->i_op = &cgroupfs_inode_ops;
    inode->i_fop = kind == CGROUPFS_INODE_DIR ? &cgroupfs_dir_file_ops
                                              : &cgroupfs_file_ops;
    vfs_inode_init_owner(inode, dir, mode);
    inode->i_nlink = kind == CGROUPFS_INODE_DIR ? 2 : 1;
    inode->i_ino = (ino64_t)(uintptr_t)inode;
    inode->inode = inode->i_ino;
    inode->i_blkbits = 12;

    if (kind == CGROUPFS_INODE_CGROUP_EVENTS) {
        spin_lock(&cgroupfs_events_lock);
        llist_append(&cgroupfs_events_inodes, &info->events_node);
        spin_unlock(&cgroupfs_events_lock);
    }
    return inode;
}

static void cgroupfs_notify_events(cgroup_t *cgroup) {
    cgroupfs_inode_info_t *info, *tmp;
    struct vfs_inode **inodes;
    size_t count = 0;
    size_t index = 0;

    if (!cgroup)
        return;

    cgroup_lock();
    spin_lock(&cgroupfs_events_lock);
    llist_for_each(info, tmp, &cgroupfs_events_inodes, events_node) {
        if (cgroup_is_descendant_of(cgroup, info->cgroup))
            count++;
    }
    spin_unlock(&cgroupfs_events_lock);
    cgroup_unlock();
    if (!count)
        return;

    inodes = calloc(count, sizeof(*inodes));
    if (!inodes)
        return;

    cgroup_lock();
    spin_lock(&cgroupfs_events_lock);
    llist_for_each(info, tmp, &cgroupfs_events_inodes, events_node) {
        if (cgroup_is_descendant_of(cgroup, info->cgroup) && index < count)
            inodes[index++] = vfs_igrab(&info->vfs_inode);
    }
    spin_unlock(&cgroupfs_events_lock);
    cgroup_unlock();

    for (size_t i = 0; i < index; i++) {
        if (!inodes[i])
            continue;
        notifyfs_queue_inode_event(inodes[i], inodes[i], NULL, IN_MODIFY, 0);
        vfs_iput(inodes[i]);
    }
    free(inodes);
}

static cgroupfs_dirent_t *cgroupfs_find_dirent(struct vfs_inode *dir,
                                               const char *name) {
    cgroupfs_dirent_t *de, *tmp;

    llist_for_each(de, tmp, &cgroupfs_i(dir)->children, node) {
        if (de->name && streq(de->name, name))
            return de;
    }
    return NULL;
}

static int cgroupfs_add_dirent(struct vfs_inode *dir, const char *name,
                               struct vfs_inode *inode) {
    cgroupfs_dirent_t *de = calloc(1, sizeof(*de));

    if (!de)
        return -ENOMEM;

    de->name = strdup(name);
    if (!de->name) {
        free(de);
        return -ENOMEM;
    }

    de->inode = vfs_igrab(inode);
    llist_init_head(&de->node);
    llist_append(&cgroupfs_i(dir)->children, &de->node);
    return 0;
}

static cgroupfs_dirent_t *cgroupfs_detach_dirent(struct vfs_inode *dir,
                                                 const char *name) {
    cgroupfs_dirent_t *de = cgroupfs_find_dirent(dir, name);

    if (!de)
        return NULL;

    llist_delete(&de->node);
    return de;
}

static uint32_t cgroupfs_available_controllers(cgroup_t *cgroup) {
    if (!cgroup || !cgroup_parent(cgroup))
        return CGROUPFS_ALL_CONTROLLERS;
    return cgroup_subtree_control(cgroup_parent(cgroup));
}

static void cgroupfs_append_controllers(string_builder_t *builder,
                                        uint32_t mask) {
    bool first = true;

    if (mask & CGROUPFS_CTRL_CPU) {
        string_builder_append(builder, "%scpu", first ? "" : " ");
        first = false;
    }
    if (mask & CGROUPFS_CTRL_IO) {
        string_builder_append(builder, "%sio", first ? "" : " ");
        first = false;
    }
    if (mask & CGROUPFS_CTRL_MEMORY) {
        string_builder_append(builder, "%smemory", first ? "" : " ");
        first = false;
    }
    if (mask & CGROUPFS_CTRL_PIDS) {
        string_builder_append(builder, "%spids", first ? "" : " ");
    }
}

static int cgroupfs_parse_controller(const char *name, uint32_t *mask) {
    if (!name || !mask)
        return -EINVAL;
    if (!strcmp(name, "cpu"))
        *mask = CGROUPFS_CTRL_CPU;
    else if (!strcmp(name, "io"))
        *mask = CGROUPFS_CTRL_IO;
    else if (!strcmp(name, "memory"))
        *mask = CGROUPFS_CTRL_MEMORY;
    else if (!strcmp(name, "pids"))
        *mask = CGROUPFS_CTRL_PIDS;
    else
        return -EINVAL;
    return 0;
}

static int cgroupfs_target_cgroup_from_fd(int fd, cgroup_t **ret_cgroup) {
    struct vfs_file *file = NULL;
    struct vfs_inode *inode = NULL;
    cgroupfs_inode_info_t *info = NULL;
    cgroup_t *cgroup = NULL;

    if (!ret_cgroup)
        return -EINVAL;
    *ret_cgroup = NULL;

    if (!current_task || fd < 0 || fd >= MAX_FD_NUM)
        return -EBADF;

    file = task_get_file(current_task, fd);
    if (!file)
        return -EBADF;

    inode = file->f_inode;
    if (!inode || !inode->i_sb ||
        (inode->i_sb->s_type != &cgroupfs_fs_type &&
         inode->i_sb->s_type != &cgroupfs_legacy_fs_type)) {
        vfs_file_put(file);
        return -EBADF;
    }

    info = cgroupfs_i(inode);
    if (!info || info->kind != CGROUPFS_INODE_DIR || !info->cgroup) {
        vfs_file_put(file);
        return -EINVAL;
    }

    cgroup = cgroup_get(info->cgroup);
    vfs_file_put(file);
    if (!cgroup)
        return -EINVAL;

    *ret_cgroup = cgroup;
    return 0;
}

int cgroupfs_set_task_cgroup_by_fd(task_t *task, int fd) {
    cgroup_t *cgroup = NULL;
    cgroup_t *old_cgroup;
    int ret;

    if (!task)
        return -ESRCH;

    ret = cgroupfs_target_cgroup_from_fd(fd, &cgroup);
    if (ret < 0)
        return ret;

    old_cgroup = cgroup_task_cgroup(task);
    cgroup_lock();
    ret = cgroup_attach_task_pid_locked(task->pid, cgroup);
    cgroup_unlock();

    if (ret == 0) {
        cgroupfs_notify_events(old_cgroup);
        if (old_cgroup != cgroup)
            cgroupfs_notify_events(cgroup);
    }

    cgroup_put(old_cgroup);
    cgroup_put(cgroup);
    return ret;
}

void cgroupfs_on_new_task(task_t *task) {
    cgroup_t *cgroup;

    cgroup_on_new_task(task);
    cgroup = cgroup_task_cgroup(task);
    cgroupfs_notify_events(cgroup);
    cgroup_put(cgroup);
}

void cgroupfs_on_exit_task(task_t *task) {
    cgroup_t *cgroup = cgroup_task_cgroup(task);

    cgroup_on_exit_task(task);
    cgroupfs_notify_events(cgroup);
    cgroup_put(cgroup);
}

static size_t cgroupfs_collect_members(cgroup_t *cgroup, bool threads,
                                       uint64_t *ids, size_t capacity) {
    size_t count = 0;

    cgroup_lock();
    spin_lock(&task_queue_lock);

    if (task_pid_map.buckets) {
        for (size_t i = 0; i < task_pid_map.bucket_count; ++i) {
            hashmap_entry_t *entry = &task_pid_map.buckets[i];
            task_t *task;
            uint64_t id;
            bool duplicate = false;

            if (!hashmap_entry_is_occupied(entry))
                continue;

            task = (task_t *)entry->value;
            if (!task || task->state == TASK_DIED)
                continue;
            if (cgroup_task_cgroup_locked(task->pid) != cgroup)
                continue;

            id = threads ? task->pid : task_effective_tgid(task);
            for (size_t j = 0; j < count; ++j) {
                if (ids[j] == id) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && count < capacity)
                ids[count++] = id;
        }
    }

    spin_unlock(&task_queue_lock);
    cgroup_unlock();
    return count;
}

static bool cgroupfs_cgroup_populated(cgroup_t *cgroup) {
    bool populated = false;

    cgroup_lock();
    spin_lock(&task_queue_lock);

    if (task_pid_map.buckets) {
        for (size_t i = 0; i < task_pid_map.bucket_count; ++i) {
            hashmap_entry_t *entry = &task_pid_map.buckets[i];
            task_t *task;
            cgroup_t *task_cgroup;

            if (!hashmap_entry_is_occupied(entry))
                continue;

            task = (task_t *)entry->value;
            if (!task || task->state == TASK_DIED)
                continue;

            task_cgroup = cgroup_task_cgroup_locked(task->pid);
            if (cgroup_is_descendant_of(task_cgroup, cgroup)) {
                populated = true;
                break;
            }
        }
    }

    spin_unlock(&task_queue_lock);
    cgroup_unlock();
    return populated;
}

static bool cgroupfs_cgroup_has_live_members(cgroup_t *cgroup) {
    bool has_members = false;

    cgroup_lock();
    spin_lock(&task_queue_lock);

    if (task_pid_map.buckets) {
        for (size_t i = 0; i < task_pid_map.bucket_count; ++i) {
            hashmap_entry_t *entry = &task_pid_map.buckets[i];
            task_t *task;

            if (!hashmap_entry_is_occupied(entry))
                continue;
            task = (task_t *)entry->value;
            if (!task || task->state == TASK_DIED)
                continue;
            if (cgroup_task_cgroup_locked(task->pid) == cgroup) {
                has_members = true;
                break;
            }
        }
    }

    spin_unlock(&task_queue_lock);
    cgroup_unlock();
    return has_members;
}

static char *cgroupfs_build_members_file(cgroup_t *cgroup, bool threads,
                                         size_t *content_len) {
    size_t max_ids = MAX((size_t)1, hashmap_size(&task_pid_map));
    uint64_t *ids = calloc(max_ids, sizeof(*ids));
    string_builder_t *builder = NULL;
    size_t count;

    if (!ids) {
        *content_len = 0;
        return NULL;
    }

    count = cgroupfs_collect_members(cgroup, threads, ids, max_ids);
    builder = create_string_builder(MAX((size_t)32, count * 24 + 1));
    if (!builder) {
        free(ids);
        *content_len = 0;
        return NULL;
    }

    for (size_t i = 0; i < count; ++i) {
        string_builder_append(builder, "%llu\n", (unsigned long long)ids[i]);
    }

    free(ids);
    *content_len = builder->size;
    char *data = builder->data;
    free(builder);
    return data;
}

static char *cgroupfs_build_file(cgroupfs_inode_info_t *info,
                                 size_t *content_len) {
    string_builder_t *builder;
    uint32_t mask;

    if (!info || !info->cgroup || !content_len)
        return NULL;

    switch (info->kind) {
    case CGROUPFS_INODE_CGROUP_PROCS:
        return cgroupfs_build_members_file(info->cgroup, false, content_len);
    case CGROUPFS_INODE_CGROUP_THREADS:
        return cgroupfs_build_members_file(info->cgroup, true, content_len);
    default:
        break;
    }

    builder = create_string_builder(128);
    if (!builder) {
        *content_len = 0;
        return NULL;
    }

    switch (info->kind) {
    case CGROUPFS_INODE_CGROUP_CONTROLLERS:
        cgroupfs_append_controllers(
            builder, cgroupfs_available_controllers(info->cgroup));
        string_builder_append(builder, "\n");
        break;
    case CGROUPFS_INODE_CGROUP_EVENTS:
        string_builder_append(builder, "populated %d\nfrozen %d\n",
                              cgroupfs_cgroup_populated(info->cgroup) ? 1 : 0,
                              cgroup_frozen(info->cgroup) ? 1 : 0);
        break;
    case CGROUPFS_INODE_CGROUP_TYPE:
        string_builder_append(builder, "domain\n");
        break;
    case CGROUPFS_INODE_CGROUP_FREEZE:
        string_builder_append(builder, "%d\n",
                              cgroup_frozen(info->cgroup) ? 1 : 0);
        break;
    case CGROUPFS_INODE_CGROUP_SUBTREE_CONTROL:
        mask = cgroup_subtree_control(info->cgroup);
        cgroupfs_append_controllers(builder, mask);
        string_builder_append(builder, "\n");
        break;
    case CGROUPFS_INODE_CGROUP_MAX_DEPTH:
    case CGROUPFS_INODE_CGROUP_MAX_DESCENDANTS:
        string_builder_append(builder, "max\n");
        break;
    case CGROUPFS_INODE_CGROUP_STAT:
        string_builder_append(
            builder,
            "nr_descendants %llu\n"
            "nr_dying_descendants 0\n",
            (unsigned long long)cgroup_descendant_count(info->cgroup));
        break;
    case CGROUPFS_INODE_CPU_STAT:
        string_builder_append(
            builder, "usage_usec %llu\n",
            (unsigned long long)(cgroup_runtime_ns(info->cgroup) / 1000));
        break;
    default:
        string_builder_append(builder, "\n");
        break;
    }

    *content_len = builder->size;
    char *data = builder->data;
    free(builder);
    return data;
}

static int cgroupfs_parse_u64(const char *buf, uint64_t *value) {
    uint64_t parsed = 0;

    if (!buf || !buf[0] || !value)
        return -EINVAL;

    while (*buf == ' ' || *buf == '\t' || *buf == '\n')
        buf++;
    if (!*buf)
        return -EINVAL;

    while (*buf >= '0' && *buf <= '9') {
        parsed = parsed * 10 + (uint64_t)(*buf - '0');
        buf++;
    }

    while (*buf == ' ' || *buf == '\t' || *buf == '\n')
        buf++;
    if (*buf)
        return -EINVAL;

    *value = parsed;
    return 0;
}

static int cgroupfs_write_procs(cgroup_t *cgroup, uint64_t pid, bool threads) {
    task_t *task = NULL;
    uint64_t *pids = NULL;
    cgroup_t **old_cgroups = NULL;
    size_t max_pids = MAX((size_t)1, hashmap_size(&task_pid_map));
    size_t count = 0;
    int ret = 0;

    task = task_find_by_pid(pid);
    if (!task || task->state == TASK_DIED)
        return -ESRCH;

    pids = calloc(max_pids, sizeof(*pids));
    old_cgroups = calloc(max_pids, sizeof(*old_cgroups));
    if (!pids || !old_cgroups) {
        free(pids);
        free(old_cgroups);
        return -ENOMEM;
    }

    spin_lock(&task_queue_lock);
    if (threads) {
        pids[count++] = pid;
    } else if (task_pid_map.buckets) {
        uint64_t tgid = task_effective_tgid(task);
        for (size_t i = 0; i < task_pid_map.bucket_count; ++i) {
            hashmap_entry_t *entry = &task_pid_map.buckets[i];
            task_t *peer;

            if (!hashmap_entry_is_occupied(entry))
                continue;
            peer = (task_t *)entry->value;
            if (!peer || peer->state == TASK_DIED)
                continue;
            if (task_effective_tgid(peer) != tgid)
                continue;
            if (count < max_pids)
                pids[count++] = peer->pid;
        }
    }
    spin_unlock(&task_queue_lock);

    /* Settle a currently running task against its old cgroup before moving it.
     */
    for (size_t i = 0; i < count; ++i) {
        task_t *member = task_find_by_pid(pids[i]);
        if (member) {
            task_account_runtime_ns(member, nano_time());
            old_cgroups[i] = cgroup_task_cgroup(member);
        }
    }

    cgroup_lock();
    for (size_t i = 0; i < count; ++i) {
        ret = cgroup_attach_task_pid_locked(pids[i], cgroup);
        if (ret < 0)
            break;
    }
    cgroup_unlock();

    if (ret == 0) {
        for (size_t i = 0; i < count; i++) {
            cgroupfs_notify_events(old_cgroups[i]);
            if (old_cgroups[i] != cgroup)
                cgroupfs_notify_events(cgroup);
        }
    }

    for (size_t i = 0; i < count; i++)
        cgroup_put(old_cgroups[i]);
    free(old_cgroups);
    free(pids);
    return ret;
}

static int cgroupfs_write_subtree_control(cgroup_t *cgroup, const char *buf) {
    char *copy = strdup(buf ? buf : "");
    char *cursor = copy;
    int ret = 0;

    if (!copy)
        return -ENOMEM;

    cgroup_lock();
    while (*cursor) {
        char *token;
        char op;
        uint32_t mask;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')
            cursor++;
        if (!*cursor)
            break;

        token = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '\n')
            cursor++;
        if (*cursor)
            *cursor++ = '\0';

        op = token[0];
        if ((op != '+' && op != '-') || !token[1]) {
            ret = -EINVAL;
            break;
        }

        ret = cgroupfs_parse_controller(token + 1, &mask);
        if (ret < 0)
            break;
        if (!(cgroupfs_available_controllers(cgroup) & mask)) {
            ret = -EINVAL;
            break;
        }

        if (op == '+')
            cgroup_set_subtree_control(cgroup,
                                       cgroup_subtree_control(cgroup) | mask);
        else
            cgroup_set_subtree_control(cgroup,
                                       cgroup_subtree_control(cgroup) & ~mask);
    }
    cgroup_unlock();

    free(copy);
    return ret;
}

static int cgroupfs_write_freeze(cgroup_t *cgroup, const char *buf) {
    uint64_t value = 0;
    int ret = cgroupfs_parse_u64(buf, &value);

    if (ret < 0)
        return ret;
    if (value > 1)
        return -EINVAL;

    cgroup_lock();
    cgroup_set_frozen(cgroup, value != 0);
    cgroup_unlock();
    return 0;
}

static int cgroupfs_setattr(struct vfs_dentry *dentry,
                            const struct vfs_kstat *stat) {
    struct vfs_inode *inode;

    if (!dentry || !dentry->d_inode || !stat)
        return -EINVAL;

    inode = dentry->d_inode;

    if (!S_ISDIR(inode->i_mode) && stat->size != inode->i_size) {
        if (stat->size != 0)
            return -EOPNOTSUPP;
        inode->i_size = 0;
    }

    if (stat->mode)
        inode->i_mode = (inode->i_mode & S_IFMT) | (stat->mode & 07777);
    inode->i_uid = stat->uid;
    inode->i_gid = stat->gid;
    inode->inode = inode->i_ino;
    return 0;
}

static struct vfs_dentry *cgroupfs_lookup(struct vfs_inode *dir,
                                          struct vfs_dentry *dentry,
                                          unsigned int flags) {
    cgroupfs_dirent_t *de;

    (void)flags;
    de = cgroupfs_find_dirent(dir, dentry->d_name.name);
    vfs_d_instantiate(dentry, de ? de->inode : NULL);
    return dentry;
}

static int cgroupfs_create_control_file(struct vfs_inode *dir, const char *name,
                                        cgroupfs_inode_kind_t kind,
                                        umode_t mode) {
    struct vfs_inode *inode = cgroupfs_new_inode(
        dir->i_sb, dir, cgroupfs_i(dir)->cgroup, kind, S_IFREG | mode);
    int ret;

    if (!inode)
        return -ENOMEM;

    ret = cgroupfs_add_dirent(dir, name, inode);
    vfs_iput(inode);
    return ret;
}

static int cgroupfs_populate_dir(struct vfs_inode *dir) {
    if (cgroupfs_create_control_file(dir, "cgroup.procs",
                                     CGROUPFS_INODE_CGROUP_PROCS, 0644) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.threads",
                                     CGROUPFS_INODE_CGROUP_THREADS, 0644) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.controllers",
                                     CGROUPFS_INODE_CGROUP_CONTROLLERS,
                                     0444) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.events",
                                     CGROUPFS_INODE_CGROUP_EVENTS, 0444) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.type",
                                     CGROUPFS_INODE_CGROUP_TYPE, 0444) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.freeze",
                                     CGROUPFS_INODE_CGROUP_FREEZE, 0644) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.subtree_control",
                                     CGROUPFS_INODE_CGROUP_SUBTREE_CONTROL,
                                     0644) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.max.depth",
                                     CGROUPFS_INODE_CGROUP_MAX_DEPTH, 0444) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.max.descendants",
                                     CGROUPFS_INODE_CGROUP_MAX_DESCENDANTS,
                                     0444) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cgroup.stat",
                                     CGROUPFS_INODE_CGROUP_STAT, 0444) < 0)
        return -ENOMEM;
    if (cgroupfs_create_control_file(dir, "cpu.stat", CGROUPFS_INODE_CPU_STAT,
                                     0444) < 0)
        return -ENOMEM;
    return 0;
}

static int cgroupfs_mkdir(struct vfs_inode *dir, struct vfs_dentry *dentry,
                          umode_t mode) {
    struct vfs_inode *inode = NULL;
    cgroup_t *parent = cgroupfs_i(dir)->cgroup;
    cgroup_t *child = NULL;
    int ret = 0;

    if (cgroupfs_find_dirent(dir, dentry->d_name.name))
        return -EEXIST;

    child = cgroup_create(parent, dentry->d_name.name);
    if (!child)
        return -ENOMEM;

    cgroup_lock();
    llist_append(cgroup_children(parent), cgroup_sibling_node(child));
    cgroup_unlock();

    inode = cgroupfs_new_inode(dir->i_sb, dir, child, CGROUPFS_INODE_DIR,
                               (mode & 07777) | S_IFDIR);
    if (!inode) {
        ret = -ENOMEM;
        goto fail;
    }

    ret = cgroupfs_populate_dir(inode);
    if (ret < 0)
        goto fail;

    ret = cgroupfs_add_dirent(dir, dentry->d_name.name, inode);
    if (ret < 0)
        goto fail;

    dir->i_nlink++;
    vfs_d_instantiate(dentry, inode);
    vfs_iput(inode);
    cgroup_put(child);
    return 0;

fail:
    cgroup_lock();
    if (!llist_empty(cgroup_sibling_node(child)))
        llist_delete(cgroup_sibling_node(child));
    cgroup_unlock();
    if (inode)
        vfs_iput(inode);
    cgroup_put(child);
    return ret;
}

static int cgroupfs_rmdir(struct vfs_inode *dir, struct vfs_dentry *dentry) {
    cgroupfs_dirent_t *de;
    cgroup_t *child;

    if (!dir || !dentry || !dentry->d_inode ||
        !S_ISDIR(dentry->d_inode->i_mode))
        return -ENOTDIR;

    child = cgroupfs_i(dentry->d_inode)->cgroup;
    if (!llist_empty(cgroup_children(child)))
        return -ENOTEMPTY;
    if (cgroupfs_cgroup_has_live_members(child))
        return -EBUSY;

    de = cgroupfs_detach_dirent(dir, dentry->d_name.name);
    if (!de)
        return -ENOENT;

    cgroup_lock();
    if (!llist_empty(cgroup_sibling_node(child)))
        llist_delete(cgroup_sibling_node(child));
    cgroup_unlock();

    if (dir->i_nlink)
        dir->i_nlink--;
    if (de->inode->i_nlink >= 2)
        de->inode->i_nlink -= 2;
    vfs_iput(de->inode);
    free(de->name);
    free(de);
    return 0;
}

static int cgroupfs_iterate_shared(struct vfs_file *file,
                                   struct vfs_dir_context *ctx) {
    cgroupfs_dirent_t *de, *tmp;
    loff_t index = 0;

    llist_for_each(de, tmp, &cgroupfs_i(file->f_inode)->children, node) {
        index++;
        if (index <= ctx->pos)
            continue;
        if (ctx->actor(ctx, de->name, (int)strlen(de->name), index,
                       de->inode->i_ino,
                       S_ISDIR(de->inode->i_mode) ? DT_DIR : DT_REG)) {
            break;
        }
        ctx->pos = index;
    }
    file->f_pos = ctx->pos;
    return 0;
}

static ssize_t cgroupfs_read(struct vfs_file *file, void *buf, size_t count,
                             loff_t *ppos) {
    cgroupfs_inode_info_t *info = cgroupfs_i(file->f_inode);
    char *content;
    size_t size = 0;
    size_t pos;
    size_t to_copy;

    if (!info || !ppos)
        return -EINVAL;

    content = cgroupfs_build_file(info, &size);
    if (!content)
        return -ENOMEM;

    file->f_inode->i_size = size;
    pos = (size_t)*ppos;
    if (pos >= size) {
        free(content);
        return 0;
    }

    to_copy = MIN(count, size - pos);
    memcpy(buf, content + pos, to_copy);
    *ppos += (loff_t)to_copy;
    free(content);
    return (ssize_t)to_copy;
}

static ssize_t cgroupfs_write(struct vfs_file *file, const void *buf,
                              size_t count, loff_t *ppos) {
    cgroupfs_inode_info_t *info = cgroupfs_i(file->f_inode);
    char *copy;
    uint64_t pid = 0;
    int ret = -EOPNOTSUPP;

    (void)ppos;
    if (!info || !info->cgroup)
        return -EINVAL;

    copy = calloc(1, count + 1);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, buf, count);

    switch (info->kind) {
    case CGROUPFS_INODE_CGROUP_PROCS:
        ret = cgroupfs_parse_u64(copy, &pid);
        if (ret == 0)
            ret = cgroupfs_write_procs(info->cgroup, pid, false);
        break;
    case CGROUPFS_INODE_CGROUP_THREADS:
        ret = cgroupfs_parse_u64(copy, &pid);
        if (ret == 0)
            ret = cgroupfs_write_procs(info->cgroup, pid, true);
        break;
    case CGROUPFS_INODE_CGROUP_SUBTREE_CONTROL:
        ret = cgroupfs_write_subtree_control(info->cgroup, copy);
        break;
    case CGROUPFS_INODE_CGROUP_FREEZE:
        ret = cgroupfs_write_freeze(info->cgroup, copy);
        break;
    default:
        ret = -EOPNOTSUPP;
        break;
    }

    free(copy);
    return ret < 0 ? ret : (ssize_t)count;
}

static int cgroupfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    file->f_op = inode->i_fop;
    return 0;
}

static int cgroupfs_init_fs_context(struct vfs_fs_context *fc) {
    cgroupfs_fs_info_t *fsi;

    if (!fc)
        return -EINVAL;

    fsi = calloc(1, sizeof(*fsi));
    if (!fsi)
        return -ENOMEM;

    fsi->unified = fc->fs_type == &cgroupfs_fs_type;
    fsi->hierarchy =
        cgroup_register_hierarchy((const char *)fc->data, fsi->unified);
    if (!fsi->hierarchy) {
        free(fsi);
        return -ENOMEM;
    }

    fc->fs_private = fsi;
    return 0;
}

static int cgroupfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    cgroupfs_fs_info_t *fsi = fc->fs_private;
    struct vfs_inode *root_inode;
    struct vfs_dentry *root_dentry;
    struct vfs_qstr root_name = {.name = "", .len = 0, .hash = 0};
    int ret;

    if (!sb)
        return -ENOMEM;

    sb->s_op = &cgroupfs_super_ops;
    sb->s_magic = 0x63677270;
    sb->s_type = fc->fs_type;
    sb->s_fs_info = fsi;

    root_inode =
        cgroupfs_new_inode(sb, NULL, cgroup_hierarchy_root(fsi->hierarchy),
                           CGROUPFS_INODE_DIR, S_IFDIR | 0755);
    if (!root_inode) {
        free(fsi);
        fc->fs_private = NULL;
        vfs_put_super(sb);
        return -ENOMEM;
    }

    ret = cgroupfs_populate_dir(root_inode);
    if (ret < 0) {
        vfs_iput(root_inode);
        free(fsi);
        fc->fs_private = NULL;
        vfs_put_super(sb);
        return ret;
    }

    root_dentry = vfs_d_alloc(sb, NULL, &root_name);
    if (!root_dentry) {
        vfs_iput(root_inode);
        free(fsi);
        fc->fs_private = NULL;
        vfs_put_super(sb);
        return -ENOMEM;
    }

    vfs_d_instantiate(root_dentry, root_inode);
    sb->s_root = root_dentry;
    fc->sb = sb;
    fc->fs_private = NULL;
    vfs_iput(root_inode);
    return 0;
}

static void cgroupfs_put_super(struct vfs_super_block *sb) {
    if (!sb)
        return;
    free(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct vfs_super_operations cgroupfs_super_ops = {
    .alloc_inode = cgroupfs_alloc_inode,
    .destroy_inode = cgroupfs_destroy_inode,
    .evict_inode = cgroupfs_evict_inode,
    .put_super = cgroupfs_put_super,
    .statfs = cgroupfs_statfs,
};

static const struct vfs_inode_operations cgroupfs_inode_ops = {
    .lookup = cgroupfs_lookup,
    .mkdir = cgroupfs_mkdir,
    .rmdir = cgroupfs_rmdir,
    .setattr = cgroupfs_setattr,
};

static const struct vfs_file_operations cgroupfs_dir_file_ops = {
    .iterate_shared = cgroupfs_iterate_shared,
    .open = cgroupfs_open,
};

static const struct vfs_file_operations cgroupfs_file_ops = {
    .read = cgroupfs_read,
    .write = cgroupfs_write,
    .open = cgroupfs_open,
};

static struct vfs_file_system_type cgroupfs_fs_type = {
    .name = "cgroup2",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = cgroupfs_init_fs_context,
    .get_tree = cgroupfs_get_tree,
};

static struct vfs_file_system_type cgroupfs_legacy_fs_type = {
    .name = "cgroup",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = cgroupfs_init_fs_context,
    .get_tree = cgroupfs_get_tree,
};

char *cgroupfs_task_path(task_t *task) { return cgroup_task_path(task); }

void cgroupfs_init(void) {
    vfs_register_filesystem(&cgroupfs_fs_type);
    vfs_register_filesystem(&cgroupfs_legacy_fs_type);
}
