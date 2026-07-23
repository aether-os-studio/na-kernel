#include <fs/proc/proc.h>
#include <arch/arch.h>
#include <boot/boot.h>
#include <init/callbacks.h>
#include <task/task.h>

#define PROCFS_MAGIC 0x9fa0ULL

typedef enum procfs_inode_kind {
    PROCFS_INO_ROOT,
    PROCFS_INO_SYS_DIR,
    PROCFS_INO_SYS_KERNEL_DIR,
    PROCFS_INO_SYS_KERNEL_RANDOM_DIR,
    PROCFS_INO_SYS_FS_DIR,
    PROCFS_INO_SYSVIPC_DIR,
    PROCFS_INO_PRESSURE_DIR,
    PROCFS_INO_TASK_DIR,
    PROCFS_INO_TASK_THREADS_DIR,
    PROCFS_INO_NS_DIR,
    PROCFS_INO_FD_DIR,
    PROCFS_INO_FDINFO_DIR,
    PROCFS_INO_FILE,
    PROCFS_INO_SYMLINK,
    PROCFS_INO_NS_HANDLE,
    PROCFS_INO_SELF,
    PROCFS_INO_THREAD_SELF,
} procfs_inode_kind_t;

typedef struct procfs_inode_info {
    struct vfs_inode vfs_inode;
    procfs_inode_kind_t kind;
    uint64_t task_pid;
    int fd_num;
    uint64_t ns_type;
    task_mount_namespace_t *mnt_ns;
    task_user_namespace_t *user_ns;
    task_simple_namespace_t *net_ns;
    char *dispatch_name;
    struct llist_header mount_watch_node;
    bool mount_watch_registered;
} procfs_inode_info_t;

typedef struct procfs_mount_watch_state {
    uint64_t seen_seq;
} procfs_mount_watch_state_t;

typedef struct procfs_task_snapshot {
    uint64_t pid;
    uint64_t tgid;
} procfs_task_snapshot_t;

static struct vfs_file_system_type procfs_fs_type;
static const struct vfs_super_operations procfs_super_ops;
static const struct vfs_dentry_operations procfs_dentry_ops;
static const struct vfs_inode_operations procfs_inode_ops;
static const struct vfs_file_operations procfs_dir_file_ops;
static const struct vfs_file_operations procfs_file_ops;

spinlock_t procfs_oplock;
static spinlock_t procfs_mount_lock;
static spinlock_t procfs_mount_watch_lock;
static struct vfs_mount *procfs_internal_mnt;
static struct llist_header procfs_mount_watch_list;
static int procfs_notify_mount_change(void);

static inline procfs_inode_info_t *procfs_i(vfs_node_t *inode) {
    return inode ? container_of(inode, procfs_inode_info_t, vfs_inode) : NULL;
}

static inline bool procfs_task_is_alive(task_t *task) {
    return task && task->state != TASK_DIED;
}

static inline bool procfs_task_is_leader(task_t *task) {
    return task && task->pid == task_effective_tgid(task);
}

static inline task_t *procfs_info_task(procfs_inode_info_t *info) {
    if (!info || info->task_pid == 0)
        return NULL;

    return task_find_by_pid(info->task_pid);
}

static size_t procfs_collect_task_snapshot(procfs_task_snapshot_t *entries,
                                           size_t capacity, uint64_t tgid) {
    size_t count = 0;

    spin_lock(&task_queue_lock);
    if (task_pid_map.buckets) {
        for (size_t i = 0; i < task_pid_map.bucket_count; ++i) {
            hashmap_entry_t *entry = &task_pid_map.buckets[i];
            if (!hashmap_entry_is_occupied(entry))
                continue;

            task_t *task = (task_t *)entry->value;
            if (!procfs_task_is_alive(task) || task->pid == 0)
                continue;

            uint64_t task_tgid = task_effective_tgid(task);
            if (tgid && task_tgid != tgid)
                continue;
            if (!tgid && task_tgid != task->pid)
                continue;

            if (entries && count < capacity) {
                entries[count].pid = task->pid;
                entries[count].tgid = task_tgid;
            }
            count++;
        }
    }
    spin_unlock(&task_queue_lock);

    return count;
}

static uint64_t procfs_task_mnt_ns_inum(task_t *task) {
    if (!task || !task->nsproxy || !task->nsproxy->mnt_ns)
        return 0;
    return task->nsproxy->mnt_ns->common.inum;
}

static uint64_t procfs_task_user_ns_inum(task_t *task) {
    if (!task || !task->nsproxy || !task->nsproxy->user_ns)
        return 0;
    return task->nsproxy->user_ns->common.inum;
}

static uint64_t procfs_task_net_ns_inum(task_t *task) {
    if (!task || !task->nsproxy || !task->nsproxy->net_ns)
        return 0;
    return task->nsproxy->net_ns->common.inum;
}

static int procfs_parse_decimal(const char *name, int *out) {
    uint64_t value = 0;

    if (!name || !name[0] || !out)
        return -EINVAL;

    while (*name) {
        if (*name < '0' || *name > '9')
            return -EINVAL;
        value = value * 10 + (uint64_t)(*name - '0');
        if (value > INT32_MAX)
            return -EINVAL;
        name++;
    }

    *out = (int)value;
    return 0;
}

static bool procfs_lookup_wants_nofollow_inode(unsigned int flags) {
    return (flags & LOOKUP_NOFOLLOW) && !(flags & LOOKUP_FOLLOW);
}

static bool procfs_is_ns_symlink_name(const char *name) {
    return name &&
           (!strcmp(name, "proc_ns_mnt") || !strcmp(name, "proc_ns_user") ||
            !strcmp(name, "proc_ns_net"));
}

static bool procfs_dispatch_is_mount_watch(const char *name) {
    return name && (!strcmp(name, "mounts") || !strcmp(name, "proc_mountinfo"));
}

static bool procfs_is_ns_link_inode(const procfs_inode_info_t *info) {
    if (!info)
        return false;
    if (info->kind == PROCFS_INO_NS_HANDLE)
        return true;
    if (info->kind != PROCFS_INO_SYMLINK)
        return false;
    return procfs_is_ns_symlink_name(info->dispatch_name);
}

static bool procfs_task_inode_is_valid(procfs_inode_info_t *info) {
    if (!info || info->task_pid == 0)
        return true;

    task_t *task = task_find_by_pid(info->task_pid);
    if (!procfs_task_is_alive(task))
        return false;

    if (info->fd_num >= 0) {
        fd_t *fd = task_get_file(task, info->fd_num);
        bool valid = fd != NULL;
        vfs_file_put(fd);
        if (!valid)
            return false;
    }

    return true;
}

static bool procfs_task_inode_is_visible(struct vfs_dentry *dentry,
                                         procfs_inode_info_t *info) {
    if (!dentry || !info || info->kind != PROCFS_INO_TASK_DIR ||
        info->task_pid == 0)
        return true;

    struct vfs_dentry *parent = dentry->d_parent;
    procfs_inode_info_t *parent_info =
        parent && parent->d_inode ? procfs_i(parent->d_inode) : NULL;
    task_t *task = procfs_info_task(info);

    if (!parent_info)
        return true;
    if (parent_info->kind == PROCFS_INO_ROOT)
        return procfs_task_is_leader(task);
    if (parent_info->kind == PROCFS_INO_TASK_THREADS_DIR) {
        task_t *owner = procfs_info_task(parent_info);
        return procfs_task_is_alive(owner) &&
               task_effective_tgid(task) == task_effective_tgid(owner);
    }

    return true;
}

static inline ssize_t procfs_copy_string(const char *str, void *addr,
                                         size_t offset, size_t size) {
    size_t len;

    if (!str)
        return -ENOENT;

    len = strlen(str);
    if (offset >= len)
        return 0;

    len = MIN(len - offset, size);
    memcpy(addr, str + offset, len);
    return (ssize_t)len;
}

static struct vfs_inode *procfs_alloc_inode(struct vfs_super_block *sb) {
    procfs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void procfs_destroy_inode(struct vfs_inode *inode) {
    if (!inode)
        return;
    free(procfs_i(inode));
}

static void procfs_evict_inode(struct vfs_inode *inode) {
    procfs_inode_info_t *info = procfs_i(inode);

    if (!info)
        return;
    if (info->mount_watch_registered) {
        spin_lock(&procfs_mount_watch_lock);
        if (!llist_empty(&info->mount_watch_node))
            llist_delete(&info->mount_watch_node);
        info->mount_watch_registered = false;
        spin_unlock(&procfs_mount_watch_lock);
    }
    free(info->dispatch_name);
    task_mount_namespace_put(info->mnt_ns);
    task_user_namespace_put(info->user_ns);
    task_simple_namespace_put(info->net_ns);
    info->mnt_ns = NULL;
    info->user_ns = NULL;
    info->net_ns = NULL;
    info->dispatch_name = NULL;
}

static int procfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static struct vfs_mount *procfs_get_internal_mount(void) {
    int ret;

    spin_lock(&procfs_mount_lock);
    if (!procfs_internal_mnt) {
        ret = vfs_kern_mount("proc", 0, NULL, NULL, &procfs_internal_mnt);
        if (ret < 0)
            procfs_internal_mnt = NULL;
    }
    if (procfs_internal_mnt)
        vfs_mntget(procfs_internal_mnt);
    spin_unlock(&procfs_mount_lock);
    return procfs_internal_mnt;
}

static uint64_t procfs_ino_for_pid(procfs_inode_kind_t kind, uint64_t pid,
                                   int fd_num, const char *name) {
    uint64_t base = ((uint64_t)kind + 1) << 56;
    uint64_t extra = 0;

    if (fd_num >= 0)
        extra = (uint64_t)(uint32_t)fd_num;
    else if (name) {
        while (*name)
            extra = extra * 131 + (unsigned char)*name++;
    }

    return base ^ (pid << 20) ^ extra;
}

static uint64_t procfs_ino_for(procfs_inode_kind_t kind, task_t *task,
                               int fd_num, const char *name) {
    return procfs_ino_for_pid(kind, task ? task->pid : 0, fd_num, name);
}

static vfs_node_t *procfs_new_inode(struct vfs_super_block *sb, umode_t mode,
                                    procfs_inode_kind_t kind, task_t *task,
                                    int fd_num, const char *dispatch_name) {
    vfs_node_t *inode = vfs_alloc_inode(sb);
    procfs_inode_info_t *info = procfs_i(inode);

    if (!inode)
        return NULL;

    inode->i_op = &procfs_inode_ops;
    inode->i_fop = S_ISDIR(mode) ? &procfs_dir_file_ops : &procfs_file_ops;
    inode->i_mode = mode;
    inode->i_uid = 0;
    inode->i_gid = 0;
    inode->i_nlink = S_ISDIR(mode) ? 2 : 1;
    inode->i_blkbits = 12;
    inode->i_ino = procfs_ino_for(kind, task, fd_num, dispatch_name);
    inode->inode = inode->i_ino;
    inode->i_atime.sec = inode->i_btime.sec = inode->i_ctime.sec =
        inode->i_mtime.sec = (int64_t)(nano_time() / 1000000000ULL);

    info->kind = kind;
    info->task_pid = task ? task->pid : 0;
    info->fd_num = fd_num;
    if (dispatch_name) {
        info->dispatch_name = strdup(dispatch_name);
        if (!info->dispatch_name) {
            vfs_iput(inode);
            return NULL;
        }
        if (procfs_dispatch_is_mount_watch(info->dispatch_name)) {
            llist_init_head(&info->mount_watch_node);
            spin_lock(&procfs_mount_watch_lock);
            llist_append(&procfs_mount_watch_list, &info->mount_watch_node);
            info->mount_watch_registered = true;
            spin_unlock(&procfs_mount_watch_lock);
        }
    }

    return inode;
}

static vfs_node_t *procfs_new_ns_inode(struct vfs_super_block *sb, task_t *task,
                                       const char *name,
                                       unsigned int lookup_flags) {
    bool want_symlink = procfs_lookup_wants_nofollow_inode(lookup_flags);
    bool is_user_ns = name && !strcmp(name, "user");
    bool is_net_ns = name && !strcmp(name, "net");
    uint64_t ns_inum = is_user_ns  ? procfs_task_user_ns_inum(task)
                       : is_net_ns ? procfs_task_net_ns_inum(task)
                                   : procfs_task_mnt_ns_inum(task);
    vfs_node_t *inode;

    if (!procfs_task_is_alive(task))
        return NULL;

    if (want_symlink) {
        return procfs_new_inode(sb, S_IFLNK | 0777, PROCFS_INO_SYMLINK, task,
                                -1,
                                is_user_ns  ? "proc_ns_user"
                                : is_net_ns ? "proc_ns_net"
                                            : "proc_ns_mnt");
    }

    inode = procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_NS_HANDLE, task, -1,
                             NULL);
    if (!inode)
        return NULL;

    procfs_i(inode)->ns_type = is_user_ns  ? CLONE_NEWUSER
                               : is_net_ns ? CLONE_NEWNET
                                           : CLONE_NEWNS;
    if (is_user_ns) {
        procfs_i(inode)->user_ns =
            task && task->nsproxy ? task->nsproxy->user_ns : NULL;
        task_user_namespace_get(procfs_i(inode)->user_ns);
    } else if (is_net_ns) {
        procfs_i(inode)->net_ns =
            task && task->nsproxy ? task->nsproxy->net_ns : NULL;
        task_simple_namespace_get(procfs_i(inode)->net_ns);
    } else {
        procfs_i(inode)->mnt_ns =
            task && task->nsproxy ? task->nsproxy->mnt_ns : NULL;
        task_mount_namespace_get(procfs_i(inode)->mnt_ns);
    }

    if (ns_inum) {
        inode->i_ino = ns_inum;
        inode->inode = ns_inum;
    }
    return inode;
}

int procfs_nsfd_identify(struct vfs_file *file, uint64_t *nstype_out,
                         task_mount_namespace_t **mnt_ns_out,
                         task_user_namespace_t **user_ns_out,
                         task_simple_namespace_t **net_ns_out) {
    procfs_inode_info_t *info;

    if (!file || !file->f_inode || !file->f_inode->i_sb ||
        file->f_inode->i_sb->s_magic != PROCFS_MAGIC) {
        return -EINVAL;
    }

    info = procfs_i(file->f_inode);
    if (!info || info->kind != PROCFS_INO_NS_HANDLE || info->ns_type == 0)
        return -EINVAL;

    if (nstype_out)
        *nstype_out = info->ns_type;
    if (mnt_ns_out)
        *mnt_ns_out = info->mnt_ns;
    if (user_ns_out)
        *user_ns_out = info->user_ns;
    if (net_ns_out)
        *net_ns_out = info->net_ns;
    return 0;
}

int procfs_create_nsfd_for_task(task_t *task, uint64_t nstype) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    const char *ns_name;
    char namebuf[64];
    int ret;

    if (!procfs_task_is_alive(task))
        return -ESRCH;

    switch (nstype) {
    case CLONE_NEWNS:
        ns_name = "mnt";
        break;
    case CLONE_NEWUSER:
        ns_name = "user";
        break;
    case CLONE_NEWNET:
        ns_name = "net";
        break;
    default:
        return -EINVAL;
    }

    mnt = procfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;

    sb = mnt->mnt_sb;
    inode = procfs_new_ns_inode(sb, task, ns_name, 0);
    if (!inode) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    snprintf(namebuf, sizeof(namebuf), "pidfd-%llu-%s-ns",
             (unsigned long long)task->pid, ns_name);
    vfs_qstr_make(&name, namebuf);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          O_RDONLY);
    if (!file) {
        vfs_dput(dentry);
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    ret = task_install_file(current_task, file, FD_CLOEXEC, 0);
    vfs_file_put(file);
    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return ret;
}

int procfs_create_nsfd_for_netns(task_simple_namespace_t *net_ns) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    char namebuf[64];
    int ret;

    if (!net_ns)
        return -EINVAL;

    mnt = procfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;

    sb = mnt->mnt_sb;
    inode = procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_NS_HANDLE, NULL, -1,
                             NULL);
    if (!inode) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    procfs_i(inode)->ns_type = CLONE_NEWNET;
    procfs_i(inode)->net_ns = net_ns;
    task_simple_namespace_get(procfs_i(inode)->net_ns);
    inode->i_ino = net_ns->common.inum;
    inode->inode = net_ns->common.inum;

    snprintf(namebuf, sizeof(namebuf), "socket-net-ns-%llu",
             (unsigned long long)net_ns->common.inum);
    vfs_qstr_make(&name, namebuf);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          O_RDONLY);
    if (!file) {
        vfs_dput(dentry);
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    ret = task_install_file(current_task, file, FD_CLOEXEC, 0);
    vfs_file_put(file);
    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return ret;
}

static vfs_node_t *procfs_lookup_path(const char *path) {
    struct vfs_path p = {0};
    vfs_node_t *inode = NULL;

    if (vfs_filename_lookup(AT_FDCWD, path, LOOKUP_FOLLOW, &p) < 0)
        return NULL;
    if (p.dentry && p.dentry->d_inode)
        inode = vfs_igrab(p.dentry->d_inode);
    vfs_path_put(&p);
    return inode;
}

static fd_t *procfs_dup_task_fd(task_t *task, int fd_num) {
    return task_get_file(task, fd_num);
}

static bool procfs_get_namespace_root(task_t *task, struct vfs_path *path) {
    struct vfs_mount *root_mnt;

    if (!path)
        return false;

    memset(path, 0, sizeof(*path));
    if (!task)
        return false;

    root_mnt = task_mount_namespace_root(task);
    if (!root_mnt || !root_mnt->mnt_root)
        return false;

    return vfs_path_set(path, root_mnt, root_mnt->mnt_root);
}

static char *procfs_path_to_task_view(task_t *task,
                                      const struct vfs_path *path) {
    const struct vfs_path *task_root;
    struct vfs_path ns_root = {0};
    char *resolved;

    if (!task || !path || !path->mnt || !path->dentry)
        return NULL;

    task_root = task_fs_root_path(task);
    if (task_root && task_root->mnt && task_root->dentry &&
        vfs_path_is_ancestor(task_root, path)) {
        resolved = vfs_path_to_string(path, task_root);
        if (resolved)
            return resolved;
    }

    if (!procfs_get_namespace_root(task, &ns_root))
        return NULL;

    resolved = vfs_path_to_string(path, &ns_root);
    vfs_path_put(&ns_root);
    return resolved;
}

static char *procfs_fd_target_path(task_t *task, fd_t *fd) {
    const char *fs_name = NULL;
    char buf[256];
    char *fullpath;

    if (!task || !fd || !fd->f_inode)
        return NULL;

    fullpath = procfs_path_to_task_view(task, &fd->f_path);
    if (fullpath)
        return fullpath;

    if (fd->f_inode->i_sb && fd->f_inode->i_sb->s_type)
        fs_name = fd->f_inode->i_sb->s_type->name;

    if (S_ISSOCK(fd->f_inode->i_mode)) {
        snprintf(buf, sizeof(buf), "socket:[%llu]",
                 (unsigned long long)fd->f_inode->i_ino);
    } else if (S_ISFIFO(fd->f_inode->i_mode)) {
        snprintf(buf, sizeof(buf), "pipe:[%llu]",
                 (unsigned long long)fd->f_inode->i_ino);
    } else if (fs_name && !strcmp(fs_name, "epollfs")) {
        snprintf(buf, sizeof(buf), "anon_inode:[eventpoll]");
    } else if (fs_name && !strcmp(fs_name, "eventfdfs")) {
        snprintf(buf, sizeof(buf), "anon_inode:[eventfd]");
    } else if (fs_name && !strcmp(fs_name, "signalfdfs")) {
        snprintf(buf, sizeof(buf), "anon_inode:[signalfd]");
    } else if (fs_name && !strcmp(fs_name, "timefdfs")) {
        snprintf(buf, sizeof(buf), "anon_inode:[timerfd]");
    } else if (fs_name && !strcmp(fs_name, "pidfdfs")) {
        snprintf(buf, sizeof(buf), "anon_inode:[pidfd]");
    } else if (fs_name && !strcmp(fs_name, "memfdfs")) {
        snprintf(buf, sizeof(buf), "memfd:[%llu]",
                 (unsigned long long)fd->f_inode->i_ino);
    } else if (fd->f_path.dentry && fd->f_path.dentry->d_name.name &&
               fd->f_path.dentry->d_name.name[0]) {
        snprintf(buf, sizeof(buf), "%s", fd->f_path.dentry->d_name.name);
    } else if (fs_name) {
        snprintf(buf, sizeof(buf), "anon_inode:[%s]", fs_name);
    } else {
        snprintf(buf, sizeof(buf), "anon_inode:[%llu]",
                 (unsigned long long)fd->f_inode->i_ino);
    }

    return strdup(buf);
}

static size_t procfs_fdinfo_render(proc_handle_t *handle, char *buf,
                                   size_t buflen) {
    task_t *task = procfs_handle_task(handle);
    fd_t *fd = procfs_dup_task_fd(task, handle ? handle->fd_num : -1);
    size_t len = 0;

    if (!buf || !buflen)
        return 0;
    buf[0] = '\0';

    if (!fd || !fd->f_inode)
        goto out;

    uint64_t flags = fd_get_flags(fd);

    len = (size_t)snprintf(buf, buflen,
                           "pos:\t%llu\n"
                           "flags:\t0%o\n"
                           "mnt_id:\t%u\n"
                           "ino:\t%llu\n",
                           (unsigned long long)fd_get_offset(fd),
                           (unsigned int)flags,
                           fd->f_path.mnt ? fd->f_path.mnt->mnt_id : 0U,
                           (unsigned long long)fd->f_inode->i_ino);
    if (len >= buflen)
        len = buflen - 1;

    if (fd->f_op && fd->f_op->show_fdinfo && len < buflen - 1) {
        size_t extra = fd->f_op->show_fdinfo(fd, buf + len, buflen - len);
        if (extra >= buflen - len)
            len = buflen - 1;
        else
            len += extra;
    }

out:
    if (fd)
        vfs_close_file(fd);
    return len;
}

static void procfs_fill_handle(proc_handle_t *handle, vfs_node_t *inode) {
    procfs_inode_info_t *info = procfs_i(inode);

    memset(handle, 0, sizeof(*handle));
    handle->node = inode;
    handle->task_pid = info ? info->task_pid : 0;
    handle->fd_num = info ? info->fd_num : -1;
    if (info && info->dispatch_name) {
        snprintf(handle->name, sizeof(handle->name), "%s", info->dispatch_name);
    }
}

static ssize_t procfs_dynamic_readlink(procfs_inode_info_t *info, void *addr,
                                       size_t offset, size_t size) {
    proc_handle_t handle;
    char path[64];
    ssize_t len;
    fd_t *fd;
    char *target;

    if (!info)
        return -EINVAL;

    switch (info->kind) {
    case PROCFS_INO_SELF:
        len = snprintf(path, sizeof(path), "%llu",
                       (unsigned long long)task_effective_tgid(current_task));
        break;
    case PROCFS_INO_THREAD_SELF:
        len = snprintf(path, sizeof(path), "%llu/task/%llu",
                       (unsigned long long)task_effective_tgid(current_task),
                       (unsigned long long)current_task->pid);
        break;
    case PROCFS_INO_SYMLINK:
        if (!info->dispatch_name)
            return -EINVAL;
        procfs_fill_handle(&handle, &info->vfs_inode);
        if (!strcmp(info->dispatch_name, "proc_root"))
            return procfs_copy_string("/", addr, offset, size);
        if (!strcmp(info->dispatch_name, "proc_exe")) {
            task_t *task = procfs_handle_task_or_current(&handle);
            if (!task || !task->exec_file)
                return -ENOENT;
            target = vfs_path_to_string(&task->exec_file->f_path,
                                        task_fs_root_path(task));
            len = procfs_copy_string(target, addr, offset, size);
            free(target);
            return len;
        }
        if (!strcmp(info->dispatch_name, "proc_fd")) {
            task_t *task = procfs_handle_task(&handle);
            fd = procfs_dup_task_fd(task, handle.fd_num);
            if (!fd)
                return -ENOENT;
            target = procfs_fd_target_path(task, fd);
            vfs_close_file(fd);
            len = procfs_copy_string(target, addr, offset, size);
            free(target);
            return len;
        }
        if (!strcmp(info->dispatch_name, "proc_ns_mnt")) {
            char nslink[64];

            snprintf(nslink, sizeof(nslink), "mnt:[%llu]",
                     (unsigned long long)procfs_task_mnt_ns_inum(
                         procfs_handle_task(&handle)));
            return procfs_copy_string(nslink, addr, offset, size);
        }
        if (!strcmp(info->dispatch_name, "proc_ns_user")) {
            char nslink[64];

            snprintf(nslink, sizeof(nslink), "user:[%llu]",
                     (unsigned long long)procfs_task_user_ns_inum(
                         procfs_handle_task(&handle)));
            return procfs_copy_string(nslink, addr, offset, size);
        }
        if (!strcmp(info->dispatch_name, "proc_ns_net")) {
            char nslink[64];

            snprintf(nslink, sizeof(nslink), "net:[%llu]",
                     (unsigned long long)procfs_task_net_ns_inum(
                         procfs_handle_task(&handle)));
            return procfs_copy_string(nslink, addr, offset, size);
        }
        return -EINVAL;
    default:
        return -EINVAL;
    }

    if (len < 0)
        return len;
    return procfs_copy_string(path, addr, offset, size);
}

static const char *procfs_get_link(struct vfs_dentry *dentry,
                                   struct vfs_inode *inode,
                                   struct vfs_nameidata *nd) {
    procfs_inode_info_t *info = procfs_i(inode);
    task_t *task;
    fd_t *fd;
    char buf[512];
    char *target;
    ssize_t len;

    (void)dentry;
    if (!info)
        return ERR_PTR(-EINVAL);

    len = procfs_dynamic_readlink(info, buf, 0, sizeof(buf) - 1);
    if (len < 0)
        return ERR_PTR((intptr_t)len);
    buf[len] = '\0';

    target = strdup(buf);
    if (!target)
        return ERR_PTR(-ENOMEM);

    if (nd) {
        memset(&nd->path, 0, sizeof(nd->path));
        task = procfs_info_task(info);
        if (!task)
            task = current_task;

        if (!task) {
            free(target);
            return ERR_PTR(-ENOENT);
        }

        if (info->kind == PROCFS_INO_SYMLINK && info->dispatch_name) {
            if (!strcmp(info->dispatch_name, "proc_root")) {
                const struct vfs_path *root = task_fs_root_path(task);

                if (!root || !root->mnt || !root->dentry) {
                    free(target);
                    return ERR_PTR(-ENOENT);
                }
                if (!vfs_path_copy(&nd->path, root)) {
                    free(target);
                    return ERR_PTR(-ENOENT);
                }
            } else if (!strcmp(info->dispatch_name, "proc_exe")) {
                if (!task->exec_file) {
                    free(target);
                    return ERR_PTR(-ENOENT);
                }
                if (!vfs_path_copy(&nd->path, &task->exec_file->f_path)) {
                    free(target);
                    return ERR_PTR(-ENOENT);
                }
            } else if (!strcmp(info->dispatch_name, "proc_fd")) {
                fd = procfs_dup_task_fd(task, info->fd_num);
                if (!fd) {
                    free(target);
                    return ERR_PTR(-ENOENT);
                }
                if (!vfs_path_copy(&nd->path, &fd->f_path)) {
                    vfs_close_file(fd);
                    free(target);
                    return ERR_PTR(-ENOENT);
                }
                vfs_close_file(fd);
            }
        }
    }

    return target;
}

static void procfs_put_link(struct vfs_inode *inode, const char *link) {
    (void)inode;
    free((void *)link);
}

static int procfs_d_revalidate(struct vfs_dentry *dentry, unsigned int flags) {
    procfs_inode_info_t *info;
    bool want_symlink;

    if (!dentry || !dentry->d_inode)
        return 1;

    info = procfs_i(dentry->d_inode);
    if (!procfs_task_inode_is_valid(info) ||
        !procfs_task_inode_is_visible(dentry, info)) {
        return 0;
    }

    if (!procfs_is_ns_link_inode(info))
        return 1;

    want_symlink = procfs_lookup_wants_nofollow_inode(flags);
    if (want_symlink)
        return info->kind == PROCFS_INO_SYMLINK ? 1 : 0;
    return info->kind == PROCFS_INO_NS_HANDLE ? 1 : 0;
}

static int procfs_permission(struct vfs_inode *inode, int mask) {
    return vfs_inode_permission(inode, mask);
}

static int procfs_getattr(const struct vfs_path *path, struct vfs_kstat *stat,
                          uint32_t request_mask, unsigned int flags) {
    procfs_inode_info_t *info;
    proc_handle_t handle;

    (void)request_mask;
    (void)flags;
    if (!path || !path->dentry || !path->dentry->d_inode)
        return -EINVAL;

    info = procfs_i(path->dentry->d_inode);
    if (!procfs_task_inode_is_valid(info))
        return -ENOENT;

    if (info && info->dispatch_name && info->kind == PROCFS_INO_FILE) {
        procfs_fill_handle(&handle, path->dentry->d_inode);
        procfs_stat_dispatch(&handle, path->dentry->d_inode);
    }

    vfs_fill_generic_kstat(path, stat);
    return 0;
}

static loff_t procfs_llseek(struct vfs_file *file, loff_t offset, int whence) {
    loff_t pos;

    if (!file || !file->f_inode)
        return -EBADF;

    spin_lock(&file->f_pos_lock);
    switch (whence) {
    case SEEK_SET:
        pos = offset;
        break;
    case SEEK_CUR:
        pos = file->f_pos + offset;
        break;
    case SEEK_END:
        pos = (loff_t)file->f_inode->i_size + offset;
        break;
    default:
        spin_unlock(&file->f_pos_lock);
        return -EINVAL;
    }
    if (pos < 0) {
        spin_unlock(&file->f_pos_lock);
        return -EINVAL;
    }
    file->f_pos = pos;
    spin_unlock(&file->f_pos_lock);
    return pos;
}

static ssize_t procfs_read(struct vfs_file *file, void *addr, size_t count,
                           loff_t *ppos) {
    proc_handle_t handle;

    if (!file || !file->f_inode || !ppos)
        return -EINVAL;
    procfs_inode_info_t *info = procfs_i(file->f_inode);
    if (!procfs_task_inode_is_valid(info))
        return -ENOENT;

    procfs_fill_handle(&handle, file->f_inode);
    if (!handle.name[0])
        return -EINVAL;
    ssize_t ret =
        (ssize_t)procfs_read_dispatch(&handle, addr, (size_t)*ppos, count);
    if (ret > 0) {
        *ppos += (loff_t)ret;
        if (procfs_dispatch_is_mount_watch(info->dispatch_name)) {
            procfs_mount_watch_state_t *state =
                (procfs_mount_watch_state_t *)file->private_data;
            if (state)
                state->seen_seq = vfs_mount_seq_read();
        }
    }
    return ret;
}

static ssize_t procfs_write(struct vfs_file *file, const void *addr,
                            size_t count, loff_t *ppos) {
    proc_handle_t handle;

    if (!file || !file->f_inode || !ppos)
        return -EINVAL;
    procfs_inode_info_t *info = procfs_i(file->f_inode);
    if (!procfs_task_inode_is_valid(info))
        return -ENOENT;

    procfs_fill_handle(&handle, file->f_inode);
    if (!handle.name[0])
        return -EINVAL;

    ssize_t ret = procfs_write_dispatch(&handle, addr, (size_t)*ppos, count);
    if (ret > 0)
        *ppos += (loff_t)ret;
    return ret;
}

static int procfs_emit_entry(struct vfs_dir_context *ctx, loff_t *index,
                             const char *name, unsigned type, uint64_t ino) {
    if (*index >= ctx->pos) {
        if (ctx->actor(ctx, name, (int)strlen(name), *index + 1, ino, type))
            return 1;
        ctx->pos = *index + 1;
    }
    (*index)++;
    return 0;
}

static int procfs_iterate_task_entries(struct vfs_dir_context *ctx, loff_t *idx,
                                       task_t *task) {
    static const struct {
        const char *name;
        unsigned type;
        procfs_inode_kind_t kind;
        const char *dispatch;
    } entries[] = {
        {"cmdline", DT_REG, PROCFS_INO_FILE, "proc_cmdline"},
        {"environ", DT_REG, PROCFS_INO_FILE, "proc_environ"},
        {"maps", DT_REG, PROCFS_INO_FILE, "proc_maps"},
        {"root", DT_LNK, PROCFS_INO_SYMLINK, "proc_root"},
        {"stat", DT_REG, PROCFS_INO_FILE, "proc_stat"},
        {"statm", DT_REG, PROCFS_INO_FILE, "proc_statm"},
        {"status", DT_REG, PROCFS_INO_FILE, "proc_status"},
        {"cgroup", DT_REG, PROCFS_INO_FILE, "proc_cgroup"},
        {"mountinfo", DT_REG, PROCFS_INO_FILE, "proc_mountinfo"},
        {"uid_map", DT_REG, PROCFS_INO_FILE, "proc_uid_map"},
        {"gid_map", DT_REG, PROCFS_INO_FILE, "proc_gid_map"},
        {"setgroups", DT_REG, PROCFS_INO_FILE, "proc_setgroups"},
        {"oom_score_adj", DT_REG, PROCFS_INO_FILE, "proc_oom_score_adj"},
        {"exe", DT_LNK, PROCFS_INO_SYMLINK, "proc_exe"},
        {"ns", DT_DIR, PROCFS_INO_NS_DIR, NULL},
        {"fd", DT_DIR, PROCFS_INO_FD_DIR, NULL},
        {"fdinfo", DT_DIR, PROCFS_INO_FDINFO_DIR, NULL},
        {"task", DT_DIR, PROCFS_INO_TASK_THREADS_DIR, NULL},
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (procfs_emit_entry(ctx, idx, entries[i].name, entries[i].type,
                              procfs_ino_for(entries[i].kind, task, -1,
                                             entries[i].dispatch)) != 0) {
            return 1;
        }
    }

    return 0;
}

static int procfs_iterate_shared(struct vfs_file *file,
                                 struct vfs_dir_context *ctx) {
    procfs_inode_info_t *info;
    loff_t index = 0;

    if (!file || !file->f_inode || !ctx)
        return -EINVAL;

    info = procfs_i(file->f_inode);
    if (!info)
        return -EINVAL;

    switch (info->kind) {
    case PROCFS_INO_ROOT: {
        if (procfs_emit_entry(
                ctx, &index, "self", DT_LNK,
                procfs_ino_for(PROCFS_INO_SELF, NULL, -1, "self")) != 0 ||
            procfs_emit_entry(ctx, &index, "thread-self", DT_LNK,
                              procfs_ino_for(PROCFS_INO_THREAD_SELF, NULL, -1,
                                             "thread-self")) != 0 ||
            procfs_emit_entry(ctx, &index, "filesystems", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "filesystems")) != 0 ||
            procfs_emit_entry(
                ctx, &index, "cgroups", DT_REG,
                procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "cgroups")) != 0 ||
            procfs_emit_entry(
                ctx, &index, "cmdline", DT_REG,
                procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "cmdline")) != 0 ||
            procfs_emit_entry(
                ctx, &index, "mounts", DT_REG,
                procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "mounts")) != 0 ||
            procfs_emit_entry(
                ctx, &index, "meminfo", DT_REG,
                procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "meminfo")) != 0 ||
            procfs_emit_entry(
                ctx, &index, "cpuinfo", DT_REG,
                procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "cpuinfo")) != 0 ||
            procfs_emit_entry(
                ctx, &index, "stat", DT_REG,
                procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "stat")) != 0 ||
            procfs_emit_entry(
                ctx, &index, "sys", DT_DIR,
                procfs_ino_for(PROCFS_INO_SYS_DIR, NULL, -1, "sys")) != 0 ||
            procfs_emit_entry(ctx, &index, "sysvipc", DT_DIR,
                              procfs_ino_for(PROCFS_INO_SYSVIPC_DIR, NULL, -1,
                                             "sysvipc")) != 0 ||
            procfs_emit_entry(ctx, &index, "pressure", DT_DIR,
                              procfs_ino_for(PROCFS_INO_PRESSURE_DIR, NULL, -1,
                                             "pressure")) != 0) {
            break;
        }

        size_t snapshot_count = procfs_collect_task_snapshot(NULL, 0, 0);
        procfs_task_snapshot_t *snapshot =
            snapshot_count ? calloc(snapshot_count, sizeof(*snapshot)) : NULL;
        if (snapshot) {
            snapshot_count =
                procfs_collect_task_snapshot(snapshot, snapshot_count, 0);
        }
        for (size_t i = 0; i < snapshot_count; ++i) {
            char name[MAX_PID_NAME_LEN];
            snprintf(name, sizeof(name), "%llu",
                     (unsigned long long)snapshot[i].pid);
            if (procfs_emit_entry(ctx, &index, name, DT_DIR,
                                  procfs_ino_for_pid(PROCFS_INO_TASK_DIR,
                                                     snapshot[i].pid, -1,
                                                     NULL)) != 0) {
                break;
            }
        }
        free(snapshot);
        break;
    }
    case PROCFS_INO_SYS_DIR:
        if (procfs_emit_entry(ctx, &index, "kernel", DT_DIR,
                              procfs_ino_for(PROCFS_INO_SYS_KERNEL_DIR, NULL,
                                             -1, "kernel")) != 0 ||
            procfs_emit_entry(
                ctx, &index, "fs", DT_DIR,
                procfs_ino_for(PROCFS_INO_SYS_FS_DIR, NULL, -1, "fs")) != 0) {
            break;
        }
        break;
    case PROCFS_INO_SYS_KERNEL_DIR:
        if (procfs_emit_entry(ctx, &index, "osrelease", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_osrelease")) !=
                0 ||
            procfs_emit_entry(ctx, &index, "random", DT_DIR,
                              procfs_ino_for(PROCFS_INO_SYS_KERNEL_RANDOM_DIR,
                                             NULL, -1, "random")) != 0 ||
            procfs_emit_entry(ctx, &index, "hostname", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_hostname")) !=
                0 ||
            procfs_emit_entry(ctx, &index, "domainname", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_domainname")) !=
                0 ||
            procfs_emit_entry(ctx, &index, "pid_max", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_pid_max")) != 0 ||
            procfs_emit_entry(ctx, &index, "tainted", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_tainted")) != 0 ||
            procfs_emit_entry(ctx, &index, "printk", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_printk")) != 0 ||
            procfs_emit_entry(ctx, &index, "cap_last_cap", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_cap_last_cap")) !=
                0 ||
            procfs_emit_entry(ctx, &index, "threads-max", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_threads_max")) !=
                0) {
            break;
        }
        break;
    case PROCFS_INO_SYS_KERNEL_RANDOM_DIR:
        if (procfs_emit_entry(ctx, &index, "boot_id", DT_REG,
                              procfs_ino_for(PROCFS_INO_FILE, NULL, -1,
                                             "proc_sys_kernel_boot_id")) != 0) {
            break;
        }
        break;
    case PROCFS_INO_SYS_FS_DIR:
        procfs_emit_entry(
            ctx, &index, "nr_open", DT_REG,
            procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "proc_sys_fs_nr_open"));
        break;
    case PROCFS_INO_SYSVIPC_DIR:
        procfs_emit_entry(
            ctx, &index, "shm", DT_REG,
            procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "proc_sysvipc_shm"));
        break;
    case PROCFS_INO_PRESSURE_DIR:
        procfs_emit_entry(
            ctx, &index, "memory", DT_REG,
            procfs_ino_for(PROCFS_INO_FILE, NULL, -1, "proc_pressure_memory"));
        break;
    case PROCFS_INO_TASK_DIR: {
        task_t *task_dir_task = procfs_info_task(info);
        if (!procfs_task_is_alive(task_dir_task))
            return -ENOENT;
        procfs_iterate_task_entries(ctx, &index, task_dir_task);
        break;
    }
    case PROCFS_INO_TASK_THREADS_DIR: {
        task_t *owner = procfs_info_task(info);
        if (!procfs_task_is_alive(owner))
            return -ENOENT;
        uint64_t tgid = task_effective_tgid(owner);
        size_t snapshot_count = procfs_collect_task_snapshot(NULL, 0, tgid);
        procfs_task_snapshot_t *snapshot =
            snapshot_count ? calloc(snapshot_count, sizeof(*snapshot)) : NULL;
        if (snapshot) {
            snapshot_count =
                procfs_collect_task_snapshot(snapshot, snapshot_count, tgid);
        }
        for (size_t i = 0; i < snapshot_count; ++i) {
            char name[MAX_PID_NAME_LEN];
            snprintf(name, sizeof(name), "%llu",
                     (unsigned long long)snapshot[i].pid);
            if (procfs_emit_entry(ctx, &index, name, DT_DIR,
                                  procfs_ino_for_pid(PROCFS_INO_TASK_DIR,
                                                     snapshot[i].pid, -1,
                                                     NULL)) != 0) {
                break;
            }
        }
        free(snapshot);
        break;
    }
    case PROCFS_INO_NS_DIR: {
        task_t *task = procfs_info_task(info);
        if (!procfs_task_is_alive(task))
            return -ENOENT;
        if (procfs_emit_entry(ctx, &index, "mnt", DT_LNK,
                              procfs_ino_for(PROCFS_INO_SYMLINK, task, -1,
                                             "proc_ns_mnt")) != 0 ||
            procfs_emit_entry(ctx, &index, "user", DT_LNK,
                              procfs_ino_for(PROCFS_INO_SYMLINK, task, -1,
                                             "proc_ns_user")) != 0 ||
            procfs_emit_entry(ctx, &index, "net", DT_LNK,
                              procfs_ino_for(PROCFS_INO_SYMLINK, task, -1,
                                             "proc_ns_net")) != 0) {
            break;
        }
        break;
    }
    case PROCFS_INO_FD_DIR:
    case PROCFS_INO_FDINFO_DIR: {
        task_t *task = procfs_info_task(info);
        if (procfs_task_is_alive(task)) {
            for (int fd_num = 0; fd_num < MAX_FD_NUM; ++fd_num) {
                char name[16];

                fd_t *fd_file = task_get_file(task, fd_num);
                if (!fd_file)
                    continue;
                snprintf(name, sizeof(name), "%d", fd_num);
                if (procfs_emit_entry(
                        ctx, &index, name,
                        info->kind == PROCFS_INO_FD_DIR ? DT_LNK : DT_REG,
                        procfs_ino_for(info->kind == PROCFS_INO_FD_DIR
                                           ? PROCFS_INO_SYMLINK
                                           : PROCFS_INO_FILE,
                                       task, fd_num,
                                       info->kind == PROCFS_INO_FD_DIR
                                           ? "proc_fd"
                                           : "proc_fdinfo")) != 0) {
                    vfs_file_put(fd_file);
                    break;
                }
                vfs_file_put(fd_file);
            }
        }
        break;
    }
    default:
        return -ENOTDIR;
    }

    file->f_pos = ctx->pos;
    return 0;
}

static int procfs_open_file(struct vfs_inode *inode, struct vfs_file *file) {
    procfs_inode_info_t *info;

    if (!inode || !file)
        return -EINVAL;
    file->f_op = inode->i_fop;

    info = procfs_i(inode);
    if (info && procfs_dispatch_is_mount_watch(info->dispatch_name)) {
        procfs_mount_watch_state_t *state = calloc(1, sizeof(*state));

        if (!state)
            return -ENOMEM;

        state->seen_seq = vfs_mount_seq_read();
        file->private_data = state;
    }

    return 0;
}

static int procfs_release_file(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    free(file ? file->private_data : NULL);
    if (file)
        file->private_data = NULL;
    return 0;
}

static __poll_t procfs_poll_file(struct vfs_file *file,
                                 struct vfs_poll_table *pt) {
    proc_handle_t handle;
    procfs_inode_info_t *info;
    if (!file || !file->f_inode)
        return EPOLLNVAL;

    info = procfs_i(file->f_inode);
    if (!info || !info->dispatch_name)
        return 0;
    if (!procfs_task_inode_is_valid(info))
        return EPOLLERR;
    if (procfs_dispatch_is_mount_watch(info->dispatch_name)) {
        procfs_mount_watch_state_t *state =
            (procfs_mount_watch_state_t *)file->private_data;
        uint64_t seq = vfs_mount_seq_read();

        if (!state)
            return EPOLLERR;
        if (state->seen_seq == seq)
            return 0;

        return EPOLLERR | EPOLLPRI;
    }

    procfs_fill_handle(&handle, file->f_inode);
    return (__poll_t)procfs_poll_dispatch(&handle, file->f_inode,
                                          pt ? (int)pt->events
                                             : (EPOLLIN | EPOLLOUT | EPOLLPRI |
                                                EPOLLERR | EPOLLHUP |
                                                EPOLLNVAL | EPOLLRDHUP));
}

static vfs_node_t *procfs_make_task_child(struct vfs_super_block *sb,
                                          const char *name, task_t *task) {
    if (!strcmp(name, "cmdline"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "proc_cmdline");
    if (!strcmp(name, "environ"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "proc_environ");
    if (!strcmp(name, "maps"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "proc_maps");
    if (!strcmp(name, "root"))
        return procfs_new_inode(sb, S_IFLNK | 0777, PROCFS_INO_SYMLINK, task,
                                -1, "proc_root");
    if (!strcmp(name, "stat"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "proc_stat");
    if (!strcmp(name, "statm"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "proc_statm");
    if (!strcmp(name, "status"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "proc_status");
    if (!strcmp(name, "cgroup"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "proc_cgroup");
    if (!strcmp(name, "mountinfo"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "proc_mountinfo");
    if (!strcmp(name, "mounts"))
        return procfs_new_inode(sb, S_IFREG | 0444, PROCFS_INO_FILE, task, -1,
                                "mounts");
    if (!strcmp(name, "uid_map"))
        return procfs_new_inode(sb, S_IFREG | 0644, PROCFS_INO_FILE, task, -1,
                                "proc_uid_map");
    if (!strcmp(name, "gid_map"))
        return procfs_new_inode(sb, S_IFREG | 0644, PROCFS_INO_FILE, task, -1,
                                "proc_gid_map");
    if (!strcmp(name, "setgroups"))
        return procfs_new_inode(sb, S_IFREG | 0644, PROCFS_INO_FILE, task, -1,
                                "proc_setgroups");
    if (!strcmp(name, "oom_score_adj"))
        return procfs_new_inode(sb, S_IFREG | 0644, PROCFS_INO_FILE, task, -1,
                                "proc_oom_score_adj");
    if (!strcmp(name, "exe"))
        return procfs_new_inode(sb, S_IFLNK | 0777, PROCFS_INO_SYMLINK, task,
                                -1, "proc_exe");
    if (!strcmp(name, "ns"))
        return procfs_new_inode(sb, S_IFDIR | 0555, PROCFS_INO_NS_DIR, task, -1,
                                NULL);
    if (!strcmp(name, "fd"))
        return procfs_new_inode(sb, S_IFDIR | 0555, PROCFS_INO_FD_DIR, task, -1,
                                NULL);
    if (!strcmp(name, "fdinfo"))
        return procfs_new_inode(sb, S_IFDIR | 0555, PROCFS_INO_FDINFO_DIR, task,
                                -1, NULL);
    if (!strcmp(name, "task"))
        return procfs_new_inode(sb, S_IFDIR | 0555, PROCFS_INO_TASK_THREADS_DIR,
                                task, -1, NULL);
    return NULL;
}

static struct vfs_dentry *procfs_lookup(struct vfs_inode *dir,
                                        struct vfs_dentry *dentry,
                                        unsigned int flags) {
    procfs_inode_info_t *info = procfs_i(dir);
    vfs_node_t *inode = NULL;
    task_t *task = NULL;
    int fd_num;

    if (!info || !dentry || !dentry->d_name.name)
        return ERR_PTR(-EINVAL);

    switch (info->kind) {
    case PROCFS_INO_ROOT:
        if (!strcmp(dentry->d_name.name, "self")) {
            inode = procfs_new_inode(dir->i_sb, S_IFLNK | 0777, PROCFS_INO_SELF,
                                     NULL, -1, NULL);
        } else if (!strcmp(dentry->d_name.name, "thread-self")) {
            inode = procfs_new_inode(dir->i_sb, S_IFLNK | 0777,
                                     PROCFS_INO_THREAD_SELF, NULL, -1, NULL);
        } else if (!strcmp(dentry->d_name.name, "filesystems")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "filesystems");
        } else if (!strcmp(dentry->d_name.name, "cgroups")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "cgroups");
        } else if (!strcmp(dentry->d_name.name, "cmdline")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "cmdline");
        } else if (!strcmp(dentry->d_name.name, "mounts")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "mounts");
        } else if (!strcmp(dentry->d_name.name, "meminfo")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "meminfo");
        } else if (!strcmp(dentry->d_name.name, "cpuinfo")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "cpuinfo");
        } else if (!strcmp(dentry->d_name.name, "stat")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "stat");
        } else if (!strcmp(dentry->d_name.name, "sys")) {
            inode = procfs_new_inode(dir->i_sb, S_IFDIR | 0555,
                                     PROCFS_INO_SYS_DIR, NULL, -1, NULL);
        } else if (!strcmp(dentry->d_name.name, "sysvipc")) {
            inode = procfs_new_inode(dir->i_sb, S_IFDIR | 0555,
                                     PROCFS_INO_SYSVIPC_DIR, NULL, -1, NULL);
        } else if (!strcmp(dentry->d_name.name, "pressure")) {
            inode = procfs_new_inode(dir->i_sb, S_IFDIR | 0555,
                                     PROCFS_INO_PRESSURE_DIR, NULL, -1, NULL);
        } else {
            uint64_t pid = 0;
            const char *name = dentry->d_name.name;
            if (name[0] >= '0' && name[0] <= '9') {
                while (*name >= '0' && *name <= '9') {
                    pid = pid * 10 + (uint64_t)(*name - '0');
                    name++;
                }
                if (!*name)
                    task = task_find_by_pid(pid);
                if (procfs_task_is_alive(task) && procfs_task_is_leader(task)) {
                    inode =
                        procfs_new_inode(dir->i_sb, S_IFDIR | 0555,
                                         PROCFS_INO_TASK_DIR, task, -1, NULL);
                }
            }
        }
        break;
    case PROCFS_INO_SYS_DIR:
        if (!strcmp(dentry->d_name.name, "kernel")) {
            inode = procfs_new_inode(dir->i_sb, S_IFDIR | 0555,
                                     PROCFS_INO_SYS_KERNEL_DIR, NULL, -1, NULL);
        } else if (!strcmp(dentry->d_name.name, "fs")) {
            inode = procfs_new_inode(dir->i_sb, S_IFDIR | 0555,
                                     PROCFS_INO_SYS_FS_DIR, NULL, -1, NULL);
        }
        break;
    case PROCFS_INO_SYS_KERNEL_DIR:
        if (!strcmp(dentry->d_name.name, "osrelease")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_osrelease");
        } else if (!strcmp(dentry->d_name.name, "random")) {
            inode = procfs_new_inode(dir->i_sb, S_IFDIR | 0555,
                                     PROCFS_INO_SYS_KERNEL_RANDOM_DIR, NULL, -1,
                                     NULL);
        } else if (!strcmp(dentry->d_name.name, "hostname")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0644, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_hostname");
        } else if (!strcmp(dentry->d_name.name, "domainname")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0644, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_domainname");
        } else if (!strcmp(dentry->d_name.name, "pid_max")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_pid_max");
        } else if (!strcmp(dentry->d_name.name, "tainted")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_tainted");
        } else if (!strcmp(dentry->d_name.name, "printk")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_printk");
        } else if (!strcmp(dentry->d_name.name, "cap_last_cap")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_cap_last_cap");
        } else if (!strcmp(dentry->d_name.name, "threads-max")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_threads_max");
        }
        break;
    case PROCFS_INO_SYS_KERNEL_RANDOM_DIR:
        if (!strcmp(dentry->d_name.name, "boot_id")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_kernel_boot_id");
        }
        break;
    case PROCFS_INO_SYS_FS_DIR:
        if (!strcmp(dentry->d_name.name, "nr_open")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sys_fs_nr_open");
        }
        break;
    case PROCFS_INO_SYSVIPC_DIR:
        if (!strcmp(dentry->d_name.name, "shm")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_sysvipc_shm");
        }
        break;
    case PROCFS_INO_PRESSURE_DIR:
        if (!strcmp(dentry->d_name.name, "memory")) {
            inode = procfs_new_inode(dir->i_sb, S_IFREG | 0444, PROCFS_INO_FILE,
                                     NULL, -1, "proc_pressure_memory");
        }
        break;
    case PROCFS_INO_TASK_DIR:
        task = procfs_info_task(info);
        if (procfs_task_is_alive(task))
            inode =
                procfs_make_task_child(dir->i_sb, dentry->d_name.name, task);
        break;
    case PROCFS_INO_TASK_THREADS_DIR: {
        uint64_t pid = 0;
        const char *name = dentry->d_name.name;
        task_t *owner = procfs_info_task(info);
        if (name[0] >= '0' && name[0] <= '9') {
            while (*name >= '0' && *name <= '9') {
                pid = pid * 10 + (uint64_t)(*name - '0');
                name++;
            }
            if (!*name)
                task = task_find_by_pid(pid);
            if (procfs_task_is_alive(task) &&
                task_effective_tgid(task) == task_effective_tgid(owner)) {
                inode = procfs_new_inode(dir->i_sb, S_IFDIR | 0555,
                                         PROCFS_INO_TASK_DIR, task, -1, NULL);
            }
        }
        break;
    }
    case PROCFS_INO_NS_DIR:
        task = procfs_info_task(info);
        if (!strcmp(dentry->d_name.name, "mnt")) {
            inode = procfs_new_ns_inode(dir->i_sb, task, dentry->d_name.name,
                                        flags);
        } else if (!strcmp(dentry->d_name.name, "user") ||
                   !strcmp(dentry->d_name.name, "net")) {
            inode = procfs_new_ns_inode(dir->i_sb, task, dentry->d_name.name,
                                        flags);
        }
        break;
    case PROCFS_INO_FD_DIR:
    case PROCFS_INO_FDINFO_DIR:
        task = procfs_info_task(info);
        if (procfs_parse_decimal(dentry->d_name.name, &fd_num) == 0 && task) {
            fd_t *fd_file = task_get_file(task, fd_num);
            bool exists = fd_file != NULL;
            vfs_file_put(fd_file);
            if (exists) {
                inode = procfs_new_inode(
                    dir->i_sb,
                    (info->kind == PROCFS_INO_FD_DIR ? S_IFLNK : S_IFREG) |
                        0444,
                    (info->kind == PROCFS_INO_FD_DIR ? PROCFS_INO_SYMLINK
                                                     : PROCFS_INO_FILE),
                    task, fd_num,
                    info->kind == PROCFS_INO_FD_DIR ? "proc_fd"
                                                    : "proc_fdinfo");
            }
        }
        break;
    default:
        break;
    }

    vfs_d_instantiate(dentry, inode);
    if (inode)
        vfs_iput(inode);
    return dentry;
}

static int procfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    vfs_node_t *root_inode;
    struct vfs_dentry *root_dentry;
    struct vfs_qstr root_name = {.name = "", .len = 0, .hash = 0};

    if (!fc)
        return -EINVAL;

    sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!sb)
        return -ENOMEM;

    sb->s_op = &procfs_super_ops;
    sb->s_d_op = &procfs_dentry_ops;
    sb->s_type = &procfs_fs_type;
    sb->s_magic = PROCFS_MAGIC;

    root_inode =
        procfs_new_inode(sb, S_IFDIR | 0555, PROCFS_INO_ROOT, NULL, -1, NULL);
    if (!root_inode) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    root_dentry = vfs_d_alloc(sb, NULL, &root_name);
    if (!root_dentry) {
        vfs_iput(root_inode);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    vfs_d_instantiate(root_dentry, root_inode);
    sb->s_root = root_dentry;
    fc->sb = sb;
    return 0;
}

static const struct vfs_super_operations procfs_super_ops = {
    .alloc_inode = procfs_alloc_inode,
    .destroy_inode = procfs_destroy_inode,
    .evict_inode = procfs_evict_inode,
};

static const struct vfs_dentry_operations procfs_dentry_ops = {
    .d_revalidate = procfs_d_revalidate,
};

static const struct vfs_inode_operations procfs_inode_ops = {
    .lookup = procfs_lookup,
    .get_link = procfs_get_link,
    .put_link = procfs_put_link,
    .permission = procfs_permission,
    .getattr = procfs_getattr,
};

static const struct vfs_file_operations procfs_dir_file_ops = {
    .llseek = procfs_llseek,
    .iterate_shared = procfs_iterate_shared,
    .open = procfs_open_file,
    .release = procfs_release_file,
    .poll = procfs_poll_file,
};

static const struct vfs_file_operations procfs_file_ops = {
    .llseek = procfs_llseek,
    .read = procfs_read,
    .write = procfs_write,
    .open = procfs_open_file,
    .release = procfs_release_file,
    .poll = procfs_poll_file,
};

static struct vfs_file_system_type procfs_fs_type = {
    .name = "proc",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = procfs_init_fs_context,
    .get_tree = procfs_get_tree,
};

ssize_t proc_root_readlink(proc_handle_t *handle, void *addr, size_t offset,
                           size_t size) {
    task_t *task;
    char *fullpath;
    ssize_t ret;

    task = procfs_handle_task_or_current(handle);
    if (!task)
        return -ENOENT;

    fullpath = procfs_path_to_task_view(task, task_fs_root_path(task));
    if (!fullpath)
        return -ENOMEM;

    ret = procfs_copy_string(fullpath, addr, offset, size);
    free(fullpath);
    return ret;
}

ssize_t proc_exe_readlink(proc_handle_t *handle, void *addr, size_t offset,
                          size_t size) {
    task_t *task;
    char *fullpath;
    ssize_t ret;

    if (!handle)
        return -EINVAL;

    task = procfs_handle_task_or_current(handle);
    if (!task || !task->exec_file)
        return -ENOENT;

    fullpath =
        vfs_path_to_string(&task->exec_file->f_path, task_fs_root_path(task));
    ret = procfs_copy_string(fullpath, addr, offset, size);
    free(fullpath);
    return ret;
}

ssize_t proc_fd_readlink(proc_handle_t *handle, void *addr, size_t offset,
                         size_t size) {
    fd_t *fd;
    char *target;
    ssize_t ret;

    if (!handle)
        return -EINVAL;

    task_t *task = procfs_handle_task(handle);
    fd = procfs_dup_task_fd(task, handle->fd_num);
    if (!fd)
        return -ENOENT;

    target = procfs_fd_target_path(task, fd);
    vfs_close_file(fd);

    ret = procfs_copy_string(target, addr, offset, size);
    free(target);
    return ret;
}

size_t proc_fdinfo_stat(proc_handle_t *handle) {
    char buf[512];
    return procfs_fdinfo_render(handle, buf, sizeof(buf));
}

size_t proc_fdinfo_read(proc_handle_t *handle, void *addr, size_t offset,
                        size_t size) {
    char buf[512];
    size_t len = procfs_fdinfo_render(handle, buf, sizeof(buf));
    return procfs_node_read(len, offset, size, addr, strdup(buf));
}

void proc_init() {
    spin_init(&procfs_oplock);
    spin_init(&procfs_mount_lock);
    spin_init(&procfs_mount_watch_lock);
    llist_init_head(&procfs_mount_watch_list);
    vfs_register_filesystem(&procfs_fs_type);
    procfs_nodes_init();
    regist_on_mount_change_callback(procfs_notify_mount_change);
}

static int procfs_notify_mount_change(void) {
    procfs_inode_info_t *info;
    procfs_inode_info_t *tmp;

    spin_lock(&procfs_mount_watch_lock);
    llist_for_each(info, tmp, &procfs_mount_watch_list, mount_watch_node) {
        vfs_poll_notify_inode(&info->vfs_inode, EPOLLERR | EPOLLPRI);
    }
    spin_unlock(&procfs_mount_watch_lock);
    return 0;
}

void procfs_on_new_task(task_t *task) { (void)task; }

void procfs_on_open_file(task_t *task, int fd) {
    (void)task;
    (void)fd;
}

void procfs_on_close_file(task_t *task, int fd) {
    (void)task;
    (void)fd;
}

void procfs_on_exit_task(task_t *task) { (void)task; }
