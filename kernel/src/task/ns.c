#include <fs/vfs/vfs.h>
#include <task/ns.h>
#include <task/task.h>
#include <arch/task_abi.h>

static uint64_t next_namespace_inum = 1;

static uint64_t task_ns_alloc_inum(void) {
    return __atomic_fetch_add(&next_namespace_inum, 1, __ATOMIC_RELAXED);
}

static void task_ns_common_init(task_ns_common_t *common) {
    if (!common)
        return;
    common->ref_count = 1;
    common->inum = task_ns_alloc_inum();
}

static void task_ns_common_get(task_ns_common_t *common) {
    if (!common)
        return;
    __atomic_add_fetch(&common->ref_count, 1, __ATOMIC_RELAXED);
}

static bool task_ns_common_put(task_ns_common_t *common) {
    if (!common)
        return false;
    return __atomic_sub_fetch(&common->ref_count, 1, __ATOMIC_ACQ_REL) == 0;
}

static int task_fs_rebind_mount_namespace(task_fs_t *fs,
                                          struct vfs_mount *old_root,
                                          struct vfs_mount *new_root) {
    struct vfs_path old_root_path = {0};
    struct vfs_path old_pwd_path = {0};
    struct vfs_path translated_root = {0};
    struct vfs_path translated_pwd = {0};

    if (!fs || !old_root || !new_root || !new_root->mnt_root)
        return -EINVAL;

    if (!vfs_path_set(&old_root_path, old_root, old_root->mnt_root))
        return -ENOENT;
    old_pwd_path = fs->vfs.pwd;
    if (vfs_translate_path_between_roots(&old_root_path, &fs->vfs.root,
                                         new_root, &translated_root) < 0 ||
        vfs_translate_path_between_roots(&old_root_path, &old_pwd_path,
                                         new_root, &translated_pwd) < 0) {
        vfs_path_put(&old_root_path);
        vfs_path_put(&translated_root);
        vfs_path_put(&translated_pwd);
        return -ENOENT;
    }

    spin_lock(&fs->vfs.lock);
    vfs_path_move(&fs->vfs.root, &translated_root);
    vfs_path_move(&fs->vfs.pwd, &translated_pwd);
    fs->vfs.seq++;
    spin_unlock(&fs->vfs.lock);
    vfs_path_put(&old_root_path);
    return 0;
}

task_fs_t *task_fs_create(const struct vfs_path *root,
                          const struct vfs_path *pwd) {
    task_fs_t *fs;

    if (!root || !root->mnt || !root->dentry)
        return NULL;

    fs = calloc(1, sizeof(*fs));
    if (!fs)
        return NULL;

    fs->ref_count = 1;
    fs->umask = 0022;
    spin_init(&fs->vfs.lock);
    fs->vfs.seq = 1;

    if (!vfs_path_copy(&fs->vfs.root, root)) {
        free(fs);
        return NULL;
    }

    if (pwd && pwd->mnt && pwd->dentry) {
        if (!vfs_path_copy(&fs->vfs.pwd, pwd)) {
            vfs_path_put(&fs->vfs.root);
            free(fs);
            return NULL;
        }
    } else {
        if (!vfs_path_copy(&fs->vfs.pwd, &fs->vfs.root)) {
            vfs_path_put(&fs->vfs.root);
            free(fs);
            return NULL;
        }
    }

    return fs;
}

void task_fs_get(task_fs_t *fs) {
    if (!fs)
        return;
    __atomic_add_fetch(&fs->ref_count, 1, __ATOMIC_RELAXED);
}

void task_fs_put(task_fs_t *fs) {
    if (!fs)
        return;
    if (__atomic_sub_fetch(&fs->ref_count, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    vfs_path_put(&fs->vfs.pwd);
    vfs_path_put(&fs->vfs.root);
    free(fs);
}

task_fs_t *task_fs_clone(task_t *task, uint64_t clone_flags) {
    task_fs_t *parent_fs;

    if (!task || !task->fs)
        return NULL;
    parent_fs = task->fs;

    if (clone_flags & CLONE_FS) {
        task_fs_get(parent_fs);
        return parent_fs;
    }

    return task_fs_create(&parent_fs->vfs.root, &parent_fs->vfs.pwd);
}

int task_fs_chdir(task_t *task, const struct vfs_path *pwd) {
    struct vfs_path new_pwd = {0};

    if (!task || !task->fs || !pwd || !pwd->mnt || !pwd->dentry)
        return -EINVAL;
    if (!pwd->dentry->d_inode || !S_ISDIR(pwd->dentry->d_inode->i_mode))
        return -ENOTDIR;
    if (!vfs_path_copy(&new_pwd, pwd))
        return -ENOENT;

    spin_lock(&task->fs->vfs.lock);
    vfs_path_move(&task->fs->vfs.pwd, &new_pwd);
    task->fs->vfs.seq++;
    spin_unlock(&task->fs->vfs.lock);
    return 0;
}

int task_fs_chroot(task_t *task, const struct vfs_path *root) {
    struct vfs_path new_root = {0};

    if (!task || !task->fs || !root || !root->mnt || !root->dentry)
        return -EINVAL;
    if (!root->dentry->d_inode || !S_ISDIR(root->dentry->d_inode->i_mode))
        return -ENOTDIR;
    if (!vfs_path_copy(&new_root, root))
        return -ENOENT;

    spin_lock(&task->fs->vfs.lock);
    vfs_path_move(&task->fs->vfs.root, &new_root);
    task->fs->vfs.seq++;
    spin_unlock(&task->fs->vfs.lock);
    return 0;
}

static void task_fs_replace_root_refs(task_fs_t *fs,
                                      const struct vfs_path *old_root,
                                      const struct vfs_path *new_root) {
    struct vfs_path replacement_root = {0};
    struct vfs_path replacement_pwd = {0};
    bool replace_root;
    bool replace_pwd;

    if (!fs || !old_root || !new_root)
        return;

    spin_lock(&fs->vfs.lock);
    replace_root = vfs_path_equal(&fs->vfs.root, old_root);
    replace_pwd = vfs_path_equal(&fs->vfs.pwd, old_root);
    if (replace_root && !vfs_path_copy(&replacement_root, new_root))
        replace_root = false;
    if (replace_pwd && !vfs_path_copy(&replacement_pwd, new_root))
        replace_pwd = false;
    if (replace_root) {
        vfs_path_move(&fs->vfs.root, &replacement_root);
    }
    if (replace_pwd) {
        vfs_path_move(&fs->vfs.pwd, &replacement_pwd);
    }
    if (replace_root || replace_pwd)
        fs->vfs.seq++;
    spin_unlock(&fs->vfs.lock);
}

static task_uts_namespace_t *
task_uts_namespace_create(const task_uts_namespace_t *parent) {
    task_uts_namespace_t *uts_ns = calloc(1, sizeof(*uts_ns));
    if (!uts_ns)
        return NULL;

    task_ns_common_init(&uts_ns->common);
    if (parent) {
        memcpy(uts_ns, parent, sizeof(*uts_ns));
        task_ns_common_init(&uts_ns->common);
        return uts_ns;
    }

    strcpy(uts_ns->sysname, "na-kernel");
    strcpy(uts_ns->nodename, "aether");
    strcpy(uts_ns->release, BUILD_VERSION);
    strcpy(uts_ns->version, BUILD_VERSION);
    strcpy(uts_ns->machine, ARCH_UTS_MACHINE);
    uts_ns->domainname[0] = '\0';
    return uts_ns;
}

static task_mount_namespace_t *
task_mount_namespace_create(struct vfs_mount *root,
                            const task_mount_namespace_t *parent) {
    task_mount_namespace_t *mnt_ns;
    struct vfs_mount *mnt_root;

    if (!root)
        return NULL;

    mnt_ns = calloc(1, sizeof(*mnt_ns));
    if (!mnt_ns)
        return NULL;

    task_ns_common_init(&mnt_ns->common);
    if (parent) {
        mnt_root = vfs_clone_mount_tree(root);
        if (!mnt_root) {
            free(mnt_ns);
            return NULL;
        }
        mnt_ns->tree_root = mnt_root;
        mnt_ns->root = vfs_mntget(mnt_root);
        if (!mnt_ns->root) {
            vfs_put_mount_tree(mnt_root);
            free(mnt_ns);
            return NULL;
        }
        mnt_ns->owns_tree = true;
    } else {
        mnt_ns->root = vfs_mntget(root);
        if (!mnt_ns->root) {
            free(mnt_ns);
            return NULL;
        }
    }

    mnt_ns->seq = parent ? parent->seq : 1;
    return mnt_ns;
}

static task_mount_namespace_t *
task_mount_namespace_create_for_task(task_t *task,
                                     const task_mount_namespace_t *parent) {
    task_mount_namespace_t *mnt_ns;
    struct vfs_mount *mnt_root = NULL;

    if (!parent)
        return task_mount_namespace_create(vfs_root_path.mnt, NULL);

    if (!parent->root)
        return task_mount_namespace_create(vfs_root_path.mnt, parent);

    mnt_ns = calloc(1, sizeof(*mnt_ns));
    if (!mnt_ns)
        return NULL;

    task_ns_common_init(&mnt_ns->common);
    mnt_root = vfs_clone_mount_tree(parent->root);
    if (!mnt_root) {
        free(mnt_ns);
        return NULL;
    }

    mnt_ns->tree_root = mnt_root;
    mnt_ns->root = vfs_mntget(mnt_root);
    if (!mnt_ns->root) {
        vfs_put_mount_tree(mnt_root);
        free(mnt_ns);
        return NULL;
    }
    mnt_ns->seq = parent->seq;
    mnt_ns->owns_tree = true;
    return mnt_ns;
}

static task_user_namespace_t *
task_user_namespace_create(const task_user_namespace_t *parent, task_t *owner) {
    task_user_namespace_t *user_ns = calloc(1, sizeof(*user_ns));
    if (!user_ns)
        return NULL;

    task_ns_common_init(&user_ns->common);
    spin_init(&user_ns->lock);
    user_ns->setgroups_state = TASK_USERNS_SETGROUPS_ALLOW;
    if (parent) {
        user_ns->level = parent->level + 1;
        if (owner) {
            user_ns->owner_uid = owner->euid;
            user_ns->owner_gid = owner->egid;
        } else {
            user_ns->owner_uid = parent->owner_uid;
            user_ns->owner_gid = parent->owner_gid;
        }
    } else if (owner) {
        user_ns->owner_uid = owner->euid;
        user_ns->owner_gid = owner->egid;
    }

    if (!parent) {
        user_ns->uid_map[0] = (task_id_map_range_t){
            .inside_id = 0, .outside_id = 0, .length = UINT32_MAX};
        user_ns->gid_map[0] = (task_id_map_range_t){
            .inside_id = 0, .outside_id = 0, .length = UINT32_MAX};
        user_ns->uid_map_count = 1;
        user_ns->gid_map_count = 1;
        user_ns->uid_map_written = true;
        user_ns->gid_map_written = true;
    }

    return user_ns;
}

static task_simple_namespace_t *
task_simple_namespace_create(const task_simple_namespace_t *parent) {
    task_simple_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns)
        return NULL;

    task_ns_common_init(&ns->common);
    (void)parent;
    return ns;
}

static void task_uts_namespace_put(task_uts_namespace_t *uts_ns) {
    if (uts_ns && task_ns_common_put(&uts_ns->common))
        free(uts_ns);
}

void task_mount_namespace_get(task_mount_namespace_t *mnt_ns) {
    task_ns_common_get(mnt_ns ? &mnt_ns->common : NULL);
}

void task_mount_namespace_put(task_mount_namespace_t *mnt_ns) {
    if (!mnt_ns)
        return;
    if (!task_ns_common_put(&mnt_ns->common))
        return;
    if (mnt_ns->owns_tree && mnt_ns->tree_root)
        vfs_put_mount_tree(mnt_ns->tree_root);
    if (mnt_ns->root)
        vfs_mntput(mnt_ns->root);
    free(mnt_ns);
}

void task_user_namespace_get(task_user_namespace_t *user_ns) {
    task_ns_common_get(user_ns ? &user_ns->common : NULL);
}

void task_user_namespace_put(task_user_namespace_t *user_ns) {
    if (user_ns && task_ns_common_put(&user_ns->common))
        free(user_ns);
}

void task_simple_namespace_get(task_simple_namespace_t *ns) {
    task_ns_common_get(ns ? &ns->common : NULL);
}

void task_simple_namespace_put(task_simple_namespace_t *ns) {
    if (ns && task_ns_common_put(&ns->common))
        free(ns);
}

task_ns_proxy_t *task_ns_proxy_create_initial(void) {
    task_ns_proxy_t *nsproxy = calloc(1, sizeof(*nsproxy));
    if (!nsproxy)
        return NULL;

    nsproxy->ref_count = 1;
    nsproxy->uts_ns = task_uts_namespace_create(NULL);
    nsproxy->mnt_ns = task_mount_namespace_create(vfs_root_path.mnt, NULL);
    nsproxy->user_ns = task_user_namespace_create(NULL, NULL);
    nsproxy->pid_ns = task_simple_namespace_create(NULL);
    nsproxy->net_ns = task_simple_namespace_create(NULL);
    nsproxy->ipc_ns = task_simple_namespace_create(NULL);
    nsproxy->cgroup_ns = task_simple_namespace_create(NULL);

    if (!nsproxy->uts_ns || !nsproxy->mnt_ns || !nsproxy->user_ns ||
        !nsproxy->pid_ns || !nsproxy->net_ns || !nsproxy->ipc_ns ||
        !nsproxy->cgroup_ns) {
        task_ns_proxy_put(nsproxy);
        return NULL;
    }

    return nsproxy;
}

void task_ns_proxy_get(task_ns_proxy_t *nsproxy) {
    if (!nsproxy)
        return;
    __atomic_add_fetch(&nsproxy->ref_count, 1, __ATOMIC_RELAXED);
}

void task_ns_proxy_put(task_ns_proxy_t *nsproxy) {
    if (!nsproxy)
        return;
    if (__atomic_sub_fetch(&nsproxy->ref_count, 1, __ATOMIC_ACQ_REL) != 0)
        return;

    task_uts_namespace_put(nsproxy->uts_ns);
    task_mount_namespace_put(nsproxy->mnt_ns);
    task_user_namespace_put(nsproxy->user_ns);
    task_simple_namespace_put(nsproxy->pid_ns);
    task_simple_namespace_put(nsproxy->net_ns);
    task_simple_namespace_put(nsproxy->ipc_ns);
    task_simple_namespace_put(nsproxy->cgroup_ns);
    free(nsproxy);
}

struct vfs_mount *task_mount_namespace_root(task_t *task) {
    if (!task || !task->nsproxy || !task->nsproxy->mnt_ns)
        return vfs_root_path.mnt;
    if (task->nsproxy->mnt_ns->root)
        return task->nsproxy->mnt_ns->root;

    {
        const struct vfs_path *fs_root = task_fs_root_path(task);

        if (fs_root && fs_root->mnt && fs_root->dentry &&
            fs_root->mnt->mnt_root == fs_root->dentry) {
            return fs_root->mnt;
        }
    }

    return vfs_root_path.mnt;
}

int task_mount_namespace_set_root(task_t *task, struct vfs_mount *root) {
    task_mount_namespace_t *mnt_ns;
    struct vfs_mount *new_root;

    if (!task || !task->nsproxy || !task->nsproxy->mnt_ns || !root)
        return -EINVAL;

    mnt_ns = task->nsproxy->mnt_ns;
    if (mnt_ns->root == root)
        return 0;

    new_root = vfs_mntget(root);
    if (!new_root)
        return -ENOENT;

    if (mnt_ns->root)
        vfs_mntput(mnt_ns->root);
    mnt_ns->root = new_root;
    mnt_ns->seq++;
    return 0;
}

int task_mount_namespace_pivot_root(task_t *task,
                                    const struct vfs_path *old_root,
                                    const struct vfs_path *new_root) {
    task_mount_namespace_t *mnt_ns;
    struct vfs_mount *new_ns_root = NULL;
    struct vfs_path new_global_root = {0};
    struct vfs_mount *new_init_root = NULL;
    bool replace_ns_root;
    bool replace_global_root;
    bool replace_init_root;

    if (!task || !task->nsproxy || !task->nsproxy->mnt_ns || !old_root ||
        !old_root->mnt || !old_root->dentry || !new_root || !new_root->mnt ||
        !new_root->dentry) {
        return -EINVAL;
    }

    mnt_ns = task->nsproxy->mnt_ns;
    replace_ns_root = mnt_ns->root != new_root->mnt;
    replace_global_root = vfs_root_path.mnt == old_root->mnt &&
                          vfs_root_path.dentry == old_root->dentry;
    replace_init_root = vfs_init_mnt_ns.root == old_root->mnt;

    if (replace_ns_root) {
        new_ns_root = vfs_mntget(new_root->mnt);
        if (!new_ns_root)
            return -ENOENT;
    }
    if (replace_global_root && !vfs_path_copy(&new_global_root, new_root)) {
        if (new_ns_root)
            vfs_mntput(new_ns_root);
        return -ENOENT;
    }
    if (replace_init_root) {
        new_init_root = vfs_mntget(new_root->mnt);
        if (!new_init_root) {
            if (new_ns_root)
                vfs_mntput(new_ns_root);
            vfs_path_put(&new_global_root);
            return -ENOENT;
        }
    }

    if (replace_ns_root) {
        if (mnt_ns->root)
            vfs_mntput(mnt_ns->root);
        mnt_ns->root = new_ns_root;
        mnt_ns->seq++;
    }

    if (replace_global_root)
        vfs_path_move(&vfs_root_path, &new_global_root);

    if (replace_init_root) {
        vfs_mntput(vfs_init_mnt_ns.root);
        vfs_init_mnt_ns.root = new_init_root;
        vfs_init_mnt_ns.seq++;
    }

    spin_lock(&task_queue_lock);
    if (task_pid_map.buckets) {
        for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
            hashmap_entry_t *entry = &task_pid_map.buckets[i];
            task_t *iter;

            if (!hashmap_entry_is_occupied(entry))
                continue;

            iter = (task_t *)entry->value;
            if (!iter || !iter->fs || !iter->nsproxy ||
                iter->nsproxy->mnt_ns != mnt_ns) {
                continue;
            }

            task_fs_replace_root_refs(iter->fs, old_root, new_root);
        }
    }
    spin_unlock(&task_queue_lock);

    return 0;
}

task_user_namespace_t *task_user_namespace_of_task(task_t *task) {
    if (!task || !task->nsproxy)
        return NULL;
    return task->nsproxy->user_ns;
}

int task_setns_mount(task_t *task, task_mount_namespace_t *target_mnt_ns) {
    task_ns_proxy_t *old_nsproxy;
    task_ns_proxy_t *new_nsproxy;
    struct vfs_mount *old_root;
    struct vfs_mount *new_root;
    int ret = 0;

    if (!task || !task->nsproxy || !target_mnt_ns || !target_mnt_ns->root)
        return -EINVAL;
    if (task->fs && task->fs->ref_count > 1)
        return -EINVAL;
    if (task->nsproxy->mnt_ns == target_mnt_ns)
        return 0;

    old_root = task_mount_namespace_root(task);
    new_root = target_mnt_ns->root;
    if (!old_root || !new_root || !new_root->mnt_root)
        return -EINVAL;

    new_nsproxy = task_ns_proxy_clone(task, 0);
    if (!new_nsproxy)
        return -ENOMEM;

    if (task->fs) {
        ret = task_fs_rebind_mount_namespace(task->fs, old_root, new_root);
        if (ret < 0) {
            task_ns_proxy_put(new_nsproxy);
            return ret;
        }
    }

    task_mount_namespace_put(new_nsproxy->mnt_ns);
    new_nsproxy->mnt_ns = target_mnt_ns;
    task_mount_namespace_get(new_nsproxy->mnt_ns);

    old_nsproxy = task->nsproxy;
    task->nsproxy = new_nsproxy;
    task_ns_proxy_put(old_nsproxy);
    return 0;
}

int task_setns_user(task_t *task, task_user_namespace_t *target_user_ns) {
    task_ns_proxy_t *old_nsproxy;
    task_ns_proxy_t *new_nsproxy;
    task_user_namespace_t *current_user_ns;

    if (!task || !task->nsproxy || !target_user_ns)
        return -EINVAL;
    if (task->fs && task->fs->ref_count > 1)
        return -EINVAL;
    if (task_thread_group_count(task_effective_tgid(task)) > 1)
        return -EINVAL;

    current_user_ns = task->nsproxy->user_ns;
    if (current_user_ns == target_user_ns)
        return -EINVAL;
    if (current_user_ns && target_user_ns->level < current_user_ns->level)
        return -EPERM;

    new_nsproxy = task_ns_proxy_clone(task, 0);
    if (!new_nsproxy)
        return -ENOMEM;

    task_user_namespace_put(new_nsproxy->user_ns);
    new_nsproxy->user_ns = target_user_ns;
    task_user_namespace_get(new_nsproxy->user_ns);

    old_nsproxy = task->nsproxy;
    task->nsproxy = new_nsproxy;
    task_ns_proxy_put(old_nsproxy);
    return 0;
}

int task_setns_net(task_t *task, task_simple_namespace_t *target_net_ns) {
    task_ns_proxy_t *old_nsproxy;
    task_ns_proxy_t *new_nsproxy;

    if (!task || !task->nsproxy || !target_net_ns)
        return -EINVAL;
    if (task->fs && task->fs->ref_count > 1)
        return -EINVAL;
    if (task->nsproxy->net_ns == target_net_ns)
        return 0;

    new_nsproxy = task_ns_proxy_clone(task, 0);
    if (!new_nsproxy)
        return -ENOMEM;

    task_simple_namespace_put(new_nsproxy->net_ns);
    new_nsproxy->net_ns = target_net_ns;
    task_simple_namespace_get(new_nsproxy->net_ns);

    old_nsproxy = task->nsproxy;
    task->nsproxy = new_nsproxy;
    task_ns_proxy_put(old_nsproxy);
    return 0;
}

task_ns_proxy_t *task_ns_proxy_clone(task_t *task, uint64_t clone_flags) {
    task_ns_proxy_t *parent;
    task_ns_proxy_t *child;

    if (!task || !task->nsproxy)
        return NULL;

    parent = task->nsproxy;
    child = calloc(1, sizeof(*child));
    if (!child)
        return NULL;
    child->ref_count = 1;

    if (clone_flags & CLONE_NEWUTS) {
        child->uts_ns = task_uts_namespace_create(parent->uts_ns);
    } else {
        child->uts_ns = parent->uts_ns;
        task_ns_common_get(&child->uts_ns->common);
    }

    if (clone_flags & CLONE_NEWNS) {
        child->mnt_ns =
            task_mount_namespace_create_for_task(task, parent->mnt_ns);
    } else {
        child->mnt_ns = parent->mnt_ns;
        task_ns_common_get(&child->mnt_ns->common);
    }

    if (clone_flags & CLONE_NEWUSER) {
        child->user_ns = task_user_namespace_create(parent->user_ns, task);
    } else {
        child->user_ns = parent->user_ns;
        task_ns_common_get(&child->user_ns->common);
    }

    if (clone_flags & CLONE_NEWPID) {
        child->pid_ns = task_simple_namespace_create(parent->pid_ns);
    } else {
        child->pid_ns = parent->pid_ns;
        task_ns_common_get(&child->pid_ns->common);
    }

    if (clone_flags & CLONE_NEWNET) {
        child->net_ns = task_simple_namespace_create(parent->net_ns);
    } else {
        child->net_ns = parent->net_ns;
        task_ns_common_get(&child->net_ns->common);
    }

    if (clone_flags & CLONE_NEWIPC) {
        child->ipc_ns = task_simple_namespace_create(parent->ipc_ns);
    } else {
        child->ipc_ns = parent->ipc_ns;
        task_ns_common_get(&child->ipc_ns->common);
    }

    if (clone_flags & CLONE_NEWCGROUP) {
        child->cgroup_ns = task_simple_namespace_create(parent->cgroup_ns);
    } else {
        child->cgroup_ns = parent->cgroup_ns;
        task_ns_common_get(&child->cgroup_ns->common);
    }

    if (!child->uts_ns || !child->mnt_ns || !child->user_ns || !child->pid_ns ||
        !child->net_ns || !child->ipc_ns || !child->cgroup_ns) {
        task_ns_proxy_put(child);
        return NULL;
    }

    return child;
}
