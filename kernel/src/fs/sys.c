#include <fs/sys.h>
#include <drivers/logger.h>
#include <mm/mm.h>
#include <net/netlink.h>

typedef struct sysfs_entry {
    struct llist_header sibling;
    struct llist_header children;
    struct sysfs_entry *parent;
    char *name;
    char *path;
    umode_t mode;
    ino64_t ino;
    char *content;
    size_t size;
    size_t capacity;
    char *link_target;
    bus_device_t *bin_device;
    bin_attribute_t *bin_attr;
} sysfs_entry_t;

typedef struct sysfs_inode_info {
    struct vfs_inode vfs_inode;
    sysfs_entry_t *entry;
    char *path;
} sysfs_inode_info_t;

static struct vfs_file_system_type sysfs_fs_type;
static const struct vfs_super_operations sysfs_super_ops;
static const struct vfs_inode_operations sysfs_inode_ops;
static const struct vfs_file_operations sysfs_dir_fops;
static const struct vfs_file_operations sysfs_file_fops;

int sysfs_fsid = 0;
spinlock_t sysfs_oplock = SPIN_INIT;
vfs_node_t *sysfs_root = NULL;
static struct vfs_mount *sysfs_mnt = NULL;
static uint32_t next_seq_num = 1;
static sysfs_entry_t sysfs_tree_root;
static bool sysfs_tree_ready = false;
static bool sysfs_internal_mounting = false;

static inline sysfs_inode_info_t *sysfs_i(vfs_node_t *inode) {
    return inode ? container_of(inode, sysfs_inode_info_t, vfs_inode) : NULL;
}

static inline sysfs_entry_t *sysfs_entry_from_inode(vfs_node_t *inode) {
    sysfs_inode_info_t *info = sysfs_i(inode);
    return info ? info->entry : NULL;
}

static char *sysfs_join_path(const char *base, const char *name) {
    size_t base_len = base ? strlen(base) : 0;
    size_t name_len = name ? strlen(name) : 0;
    bool sep = base_len && strcmp(base, "/") != 0;
    char *out = malloc(base_len + sep + name_len + 1);
    size_t pos = 0;

    if (!out)
        return NULL;

    if (base_len) {
        memcpy(out, base, base_len);
        pos += base_len;
    }
    if (sep)
        out[pos++] = '/';
    if (name_len) {
        memcpy(out + pos, name, name_len);
        pos += name_len;
    }
    out[pos] = '\0';
    return out;
}

static void sysfs_registry_init(void) {
    if (sysfs_tree_ready)
        return;

    memset(&sysfs_tree_root, 0, sizeof(sysfs_tree_root));
    llist_init_head(&sysfs_tree_root.sibling);
    llist_init_head(&sysfs_tree_root.children);
    sysfs_tree_root.name = "sys";
    sysfs_tree_root.path = "/sys";
    sysfs_tree_root.mode = S_IFDIR | 0755;
    sysfs_tree_root.ino = (ino64_t)(uintptr_t)&sysfs_tree_root;
    sysfs_tree_ready = true;
}

static sysfs_entry_t *sysfs_find_child(sysfs_entry_t *dir, const char *name) {
    sysfs_entry_t *entry, *tmp;

    if (!dir || !S_ISDIR(dir->mode) || !name)
        return NULL;

    llist_for_each(entry, tmp, &dir->children, sibling) {
        if (entry->name && streq(entry->name, name))
            return entry;
    }
    return NULL;
}

static bool sysfs_mode_matches(sysfs_entry_t *entry, umode_t mode) {
    if (!entry)
        return false;
    return ((entry->mode ^ mode) & S_IFMT) == 0;
}

static sysfs_entry_t *sysfs_new_entry(sysfs_entry_t *parent, const char *name,
                                      umode_t mode, const char *target) {
    sysfs_entry_t *entry;

    if (!parent || !name || !name[0] || !S_ISDIR(parent->mode))
        return NULL;

    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return NULL;

    entry->name = strdup(name);
    if (!entry->name)
        goto fail;

    entry->path = sysfs_join_path(parent->path, name);
    if (!entry->path)
        goto fail;

    if (S_ISLNK(mode) && target) {
        entry->link_target = strdup(target);
        if (!entry->link_target)
            goto fail;
    }

    entry->parent = parent;
    entry->mode = mode;
    entry->ino = (ino64_t)(uintptr_t)entry;
    llist_init_head(&entry->sibling);
    llist_init_head(&entry->children);
    llist_append(&parent->children, &entry->sibling);
    return entry;

fail:
    free(entry->link_target);
    free(entry->path);
    free(entry->name);
    free(entry);
    return NULL;
}

static sysfs_entry_t *sysfs_lookup_entry(const char *path) {
    sysfs_entry_t *current;
    const char *cursor;

    sysfs_registry_init();

    if (!path)
        return NULL;
    if (streq(path, "/sys"))
        return &sysfs_tree_root;
    if (strncmp(path, "/sys/", 5) != 0)
        return NULL;

    current = &sysfs_tree_root;
    cursor = path + 5;

    while (*cursor) {
        const char *sep = strchr(cursor, '/');
        size_t len = sep ? (size_t)(sep - cursor) : strlen(cursor);
        char *name;

        if (len == 0) {
            if (!sep)
                break;
            cursor = sep + 1;
            continue;
        }

        name = malloc(len + 1);
        if (!name)
            return NULL;
        memcpy(name, cursor, len);
        name[len] = '\0';

        current = sysfs_find_child(current, name);
        free(name);
        if (!current)
            return NULL;

        if (!sep)
            break;
        cursor = sep + 1;
    }

    return current;
}

static sysfs_entry_t *sysfs_ensure_entry(const char *path, umode_t mode,
                                         const char *target) {
    sysfs_entry_t *current;
    const char *cursor;

    sysfs_registry_init();

    if (!path)
        return NULL;
    if (streq(path, "/sys"))
        return S_ISDIR(mode) ? &sysfs_tree_root : NULL;
    if (strncmp(path, "/sys/", 5) != 0)
        return NULL;

    current = &sysfs_tree_root;
    cursor = path + 5;

    while (*cursor) {
        const char *sep = strchr(cursor, '/');
        size_t len = sep ? (size_t)(sep - cursor) : strlen(cursor);
        bool last = !sep;
        char *name;
        sysfs_entry_t *child;
        umode_t want_mode = last ? mode : (S_IFDIR | 0755);

        if (len == 0) {
            if (!sep)
                break;
            cursor = sep + 1;
            continue;
        }
        if (!S_ISDIR(current->mode))
            return NULL;

        name = malloc(len + 1);
        if (!name)
            return NULL;
        memcpy(name, cursor, len);
        name[len] = '\0';

        child = sysfs_find_child(current, name);
        if (!child) {
            child =
                sysfs_new_entry(current, name, want_mode, last ? target : NULL);
        } else if (!sysfs_mode_matches(child, want_mode)) {
            child = NULL;
        } else if (last && S_ISLNK(mode) && target && !child->link_target) {
            child->link_target = strdup(target);
            if (!child->link_target)
                child = NULL;
        }

        free(name);
        if (!child)
            return NULL;

        current = child;
        if (!sep)
            break;
        cursor = sep + 1;
    }

    return current;
}

static vfs_node_t *sysfs_new_inode(struct vfs_super_block *sb,
                                   sysfs_entry_t *entry) {
    vfs_node_t *inode = vfs_alloc_inode(sb);
    sysfs_inode_info_t *info;

    if (!inode || !entry)
        return NULL;
    info = sysfs_i(inode);

    inode->i_op = &sysfs_inode_ops;
    inode->i_ino = entry->ino;
    inode->inode = entry->ino;
    inode->i_mode = entry->mode;
    inode->i_nlink = S_ISDIR(entry->mode) ? 2 : 1;
    inode->i_fop = S_ISDIR(entry->mode) ? &sysfs_dir_fops : &sysfs_file_fops;
    inode->i_size = entry->size;
    info->entry = entry;
    info->path = entry->path ? strdup(entry->path) : NULL;
    if (entry->path && !info->path) {
        vfs_iput(inode);
        return NULL;
    }
    return inode;
}

static vfs_node_t *sysfs_export_entry(sysfs_entry_t *entry) {
    struct vfs_super_block *sb = NULL;

    if (!entry)
        return NULL;
    if (sysfs_root && sysfs_root->i_sb)
        sb = sysfs_root->i_sb;
    else if (sysfs_mnt && sysfs_mnt->mnt_root && sysfs_mnt->mnt_root->d_inode)
        sb = sysfs_mnt->mnt_root->d_inode->i_sb;
    if (!sb)
        return NULL;
    return sysfs_new_inode(sb, entry);
}

static int sysfs_create_common(struct vfs_inode *dir, struct vfs_dentry *dentry,
                               umode_t mode, const char *target) {
    sysfs_entry_t *parent;
    sysfs_entry_t *entry;
    vfs_node_t *inode;

    if (!dir || !dentry || !dentry->d_name.name)
        return -EINVAL;

    parent = sysfs_entry_from_inode(dir);
    if (!parent || !S_ISDIR(parent->mode))
        return -ENOTDIR;
    if (sysfs_find_child(parent, dentry->d_name.name))
        return -EEXIST;

    entry = sysfs_new_entry(parent, dentry->d_name.name, mode, target);
    if (!entry)
        return -ENOMEM;

    inode = sysfs_new_inode(dir->i_sb, entry);
    if (!inode)
        return -ENOMEM;

    vfs_d_instantiate(dentry, inode);
    vfs_iput(inode);
    return 0;
}

static ssize_t sysfs_entry_read(sysfs_entry_t *entry, void *buf, size_t count,
                                loff_t *ppos) {
    size_t offset;
    size_t to_copy;

    if (!entry || !ppos)
        return -EINVAL;
    if (entry->bin_attr && entry->bin_device) {
        ssize_t ret = entry->bin_attr->read(entry->bin_device, entry->bin_attr,
                                            buf, (uint64_t)*ppos, count);
        if (ret < 0)
            return ret;
        *ppos += ret;
        return ret;
    }

    offset = (size_t)*ppos;
    if (offset >= entry->size)
        return 0;

    to_copy = MIN(count, entry->size - offset);
    memcpy(buf, entry->content + offset, to_copy);
    *ppos += (loff_t)to_copy;
    return (ssize_t)to_copy;
}

static ssize_t sysfs_entry_write(sysfs_entry_t *entry, const void *buf,
                                 size_t count, loff_t *ppos) {
    size_t offset;
    size_t need;

    if (!entry || !ppos)
        return -EINVAL;
    if (entry->bin_attr && entry->bin_device) {
        return entry->bin_attr->write(entry->bin_device, entry->bin_attr, buf,
                                      (uint64_t)*ppos, count);
    }

    offset = (size_t)*ppos;
    need = offset + count;
    if (need > entry->capacity) {
        size_t new_cap = PADDING_UP(need, PAGE_SIZE);
        char *new_buf = alloc_frames_bytes(new_cap);

        if (!new_buf)
            return -ENOMEM;
        memset(new_buf, 0, new_cap);
        if (entry->content && entry->size)
            memcpy(new_buf, entry->content, entry->size);
        if (entry->content)
            free_frames_bytes(entry->content, entry->capacity);
        entry->content = new_buf;
        entry->capacity = new_cap;
    }

    memcpy(entry->content + offset, buf, count);
    entry->size = MAX(entry->size, need);
    *ppos += (loff_t)count;
    return (ssize_t)count;
}

static bool sysfs_is_uevent_entry(sysfs_entry_t *entry) {
    return entry && entry->parent && entry->name &&
           strcmp(entry->name, "uevent") == 0 && entry->parent->path &&
           strncmp(entry->parent->path, "/sys/", 5) == 0;
}

static bool sysfs_uevent_write_requests_add(const void *buf, size_t count) {
    const char *s = (const char *)buf;

    if (!s || count == 0)
        return false;
    while (count > 0 && (*s == ' ' || *s == '\t' || *s == '\n')) {
        s++;
        count--;
    }
    return count >= 3 && s[0] == 'a' && s[1] == 'd' && s[2] == 'd' &&
           (count == 3 || s[3] == '\0' || s[3] == '\n' || s[3] == ' ' ||
            s[3] == '\t');
}

static ssize_t sysfs_uevent_write(sysfs_entry_t *entry, const void *buf,
                                  size_t count) {
    if (!sysfs_is_uevent_entry(entry))
        return -EINVAL;
    if (!sysfs_uevent_write_requests_add(buf, count))
        return (ssize_t)count;

    const char *devpath = entry->parent->path + 4;
    const char *content = entry->content ? entry->content : "";
    int seqnum = alloc_seq_num();
    size_t buffer_len =
        snprintf(NULL, 0, "add@%s\nACTION=add\nSEQNUM=%d\n%s%s", devpath,
                 seqnum, content,
                 content[0] && content[strlen(content) - 1] == '\n' ? ""
                                                                    : "\n") +
        1;
    char *event_buffer = malloc(buffer_len);
    if (!event_buffer)
        return -ENOMEM;

    snprintf(event_buffer, buffer_len, "add@%s\nACTION=add\nSEQNUM=%d\n%s%s",
             devpath, seqnum, content,
             content[0] && content[strlen(content) - 1] == '\n' ? "" : "\n");

    size_t src_len = strlen(event_buffer);
    size_t dst_len = 0;
    bool last_was_nul = false;
    for (size_t i = 0; i < src_len; i++) {
        char c = event_buffer[i];
        if (c == '\n')
            c = '\0';
        if (c == '\0') {
            if (last_was_nul)
                continue;
            last_was_nul = true;
        } else {
            last_was_nul = false;
        }
        event_buffer[dst_len++] = c;
    }
    if (dst_len == 0 || event_buffer[dst_len - 1] != '\0')
        event_buffer[dst_len++] = '\0';

    netlink_kernel_uevent_send(event_buffer, (int)dst_len);
    free(event_buffer);
    return (ssize_t)count;
}

static struct vfs_dentry *sysfs_lookup(struct vfs_inode *dir,
                                       struct vfs_dentry *dentry,
                                       unsigned int flags) {
    sysfs_entry_t *entry;
    vfs_node_t *inode = NULL;

    (void)flags;
    if (!dir || !dentry || !dentry->d_name.name)
        return ERR_PTR(-EINVAL);

    entry = sysfs_find_child(sysfs_entry_from_inode(dir), dentry->d_name.name);
    if (entry) {
        inode = sysfs_new_inode(dir->i_sb, entry);
        if (!inode)
            return ERR_PTR(-ENOMEM);
    }

    vfs_d_instantiate(dentry, inode);
    if (inode)
        vfs_iput(inode);
    return dentry;
}

static int sysfs_create(struct vfs_inode *dir, struct vfs_dentry *dentry,
                        umode_t mode, bool excl) {
    (void)excl;
    return sysfs_create_common(dir, dentry, (mode & 0777) | S_IFREG, NULL);
}

static int sysfs_mkdir_inode(struct vfs_inode *dir, struct vfs_dentry *dentry,
                             umode_t mode) {
    return sysfs_create_common(dir, dentry, (mode & 0777) | S_IFDIR, NULL);
}

static int sysfs_symlink_inode(struct vfs_inode *dir, struct vfs_dentry *dentry,
                               const char *target) {
    return sysfs_create_common(dir, dentry, S_IFLNK | 0777, target);
}

static const char *sysfs_get_link(struct vfs_dentry *dentry,
                                  struct vfs_inode *inode,
                                  struct vfs_nameidata *nd) {
    sysfs_entry_t *entry = sysfs_entry_from_inode(inode);

    (void)dentry;
    (void)nd;
    return entry ? entry->link_target : NULL;
}

static ssize_t sysfs_read(struct vfs_file *file, void *buf, size_t count,
                          loff_t *ppos) {
    return sysfs_entry_read(sysfs_entry_from_inode(file ? file->f_inode : NULL),
                            buf, count, ppos);
}

static ssize_t sysfs_write(struct vfs_file *file, const void *buf, size_t count,
                           loff_t *ppos) {
    sysfs_entry_t *entry = sysfs_entry_from_inode(file ? file->f_inode : NULL);
    if (sysfs_is_uevent_entry(entry))
        return sysfs_uevent_write(entry, buf, count);

    ssize_t ret = sysfs_entry_write(entry, buf, count, ppos);

    if (ret >= 0 && file && file->f_inode && entry)
        file->f_inode->i_size = entry->size;
    return ret;
}

static int sysfs_iterate_shared(struct vfs_file *file,
                                struct vfs_dir_context *ctx) {
    sysfs_entry_t *dir = sysfs_entry_from_inode(file ? file->f_inode : NULL);
    sysfs_entry_t *entry, *tmp;
    loff_t pos = 0;

    if (!dir || !S_ISDIR(dir->mode) || !ctx || !ctx->actor)
        return -EINVAL;

    llist_for_each(entry, tmp, &dir->children, sibling) {
        if (pos++ < ctx->pos)
            continue;
        if (ctx->actor(ctx, entry->name, (int)strlen(entry->name), pos,
                       entry->ino,
                       S_ISDIR(entry->mode)   ? DT_DIR
                       : S_ISLNK(entry->mode) ? DT_LNK
                                              : DT_REG)) {
            break;
        }
        ctx->pos = pos;
    }

    file->f_pos = ctx->pos;
    return 0;
}

static int sysfs_open_file(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;
    file->f_op = inode->i_fop;
    return 0;
}

static int sysfs_release_file(struct vfs_inode *inode, struct vfs_file *file) {
    (void)inode;
    (void)file;
    return 0;
}

static __poll_t sysfs_poll_file(struct vfs_file *file,
                                struct vfs_poll_table *pt) {
    (void)file;
    (void)pt;
    return EPOLLIN | EPOLLOUT;
}

static struct vfs_inode *sysfs_alloc_inode(struct vfs_super_block *sb) {
    sysfs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void sysfs_destroy_inode(struct vfs_inode *inode) {
    sysfs_inode_info_t *info;

    if (!inode)
        return;
    info = sysfs_i(inode);
    if (!info)
        return;
    free(info->path);
    free(info);
}

static void sysfs_evict_inode(struct vfs_inode *inode) { (void)inode; }

static int sysfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int sysfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    vfs_node_t *root_inode;
    struct vfs_dentry *root_dentry;
    struct vfs_qstr root_name = {.name = "", .len = 0, .hash = 0};

    sysfs_registry_init();

    sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!sb)
        return -ENOMEM;

    sb->s_op = &sysfs_super_ops;
    sb->s_type = &sysfs_fs_type;
    sb->s_magic = 0x62656572;

    root_inode = sysfs_new_inode(sb, &sysfs_tree_root);
    if (!root_inode)
        return -ENOMEM;

    root_dentry = vfs_d_alloc(sb, NULL, &root_name);
    if (!root_dentry) {
        vfs_iput(root_inode);
        return -ENOMEM;
    }

    vfs_d_instantiate(root_dentry, root_inode);
    sb->s_root = root_dentry;
    fc->sb = sb;
    if (!sysfs_internal_mounting)
        sysfs_root = vfs_igrab(root_inode);
    vfs_iput(root_inode);
    return 0;
}

static int sysfs_ensure_internal_mount(void) {
    int ret;

    if (sysfs_mnt)
        return 0;

    sysfs_internal_mounting = true;
    ret = vfs_kern_mount("sysfs", 0, NULL, NULL, &sysfs_mnt);
    sysfs_internal_mounting = false;
    if (ret < 0)
        sysfs_mnt = NULL;
    return ret;
}

static const struct vfs_super_operations sysfs_super_ops = {
    .alloc_inode = sysfs_alloc_inode,
    .destroy_inode = sysfs_destroy_inode,
    .evict_inode = sysfs_evict_inode,
};

static const struct vfs_inode_operations sysfs_inode_ops = {
    .lookup = sysfs_lookup,
    .create = sysfs_create,
    .mkdir = sysfs_mkdir_inode,
    .symlink = sysfs_symlink_inode,
    .get_link = sysfs_get_link,
};

static const struct vfs_file_operations sysfs_dir_fops = {
    .iterate_shared = sysfs_iterate_shared,
    .open = sysfs_open_file,
    .release = sysfs_release_file,
    .poll = sysfs_poll_file,
};

static const struct vfs_file_operations sysfs_file_fops = {
    .read = sysfs_read,
    .write = sysfs_write,
    .open = sysfs_open_file,
    .release = sysfs_release_file,
    .poll = sysfs_poll_file,
};

static struct vfs_file_system_type sysfs_fs_type = {
    .name = "sysfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = sysfs_init_fs_context,
    .get_tree = sysfs_get_tree,
};

static vfs_node_t *sysfs_lookup_path(const char *path) {
    return sysfs_export_entry(sysfs_lookup_entry(path));
}

static char *sysfs_absolute_path(vfs_node_t *start, const char *path) {
    sysfs_inode_info_t *info;

    if (!path)
        return NULL;
    if (path[0] == '/')
        return strdup(path);

    info = sysfs_i(start);
    if (!info || !info->path)
        return NULL;
    return sysfs_join_path(info->path, path);
}

int sysfs_write_node(vfs_node_t *node, const void *buf, size_t len,
                     size_t offset) {
    sysfs_entry_t *entry = sysfs_entry_from_inode(node);
    loff_t pos = (loff_t)offset;
    ssize_t ret = sysfs_entry_write(entry, buf, len, &pos);

    if (ret >= 0 && node && entry)
        node->i_size = entry->size;
    return (int)ret;
}

char *sysfs_node_path(vfs_node_t *node) {
    sysfs_inode_info_t *info = sysfs_i(node);
    return info && info->path ? strdup(info->path) : NULL;
}

int alloc_seq_num(void) {
    return (int)__atomic_fetch_add(&next_seq_num, 1, __ATOMIC_RELAXED);
}

vfs_node_t *sysfs_ensure_dir_at(vfs_node_t *start, const char *path) {
    char *abs = sysfs_absolute_path(start, path);
    sysfs_entry_t *entry;

    if (!abs)
        return NULL;
    entry = sysfs_ensure_entry(abs, S_IFDIR | 0755, NULL);
    free(abs);
    return sysfs_export_entry(entry);
}

vfs_node_t *sysfs_ensure_dir(const char *path) {
    return sysfs_ensure_dir_at(NULL, path);
}

vfs_node_t *sysfs_ensure_file_at(vfs_node_t *start, const char *path) {
    char *abs = sysfs_absolute_path(start, path);
    sysfs_entry_t *entry;

    if (!abs)
        return NULL;
    entry = sysfs_ensure_entry(abs, S_IFREG | 0644, NULL);
    free(abs);
    return sysfs_export_entry(entry);
}

vfs_node_t *sysfs_ensure_file(const char *path) {
    return sysfs_ensure_file_at(NULL, path);
}

vfs_node_t *sysfs_ensure_symlink_at(vfs_node_t *start, const char *path,
                                    const char *target) {
    char *abs = sysfs_absolute_path(start, path);
    sysfs_entry_t *entry;

    if (!abs)
        return NULL;

    char output[256] = {0};
    calculate_relative_path(output, abs, target, sizeof(output));

    entry = sysfs_ensure_entry(abs, S_IFLNK | 0777, output);
    free(abs);
    return sysfs_export_entry(entry);
}

vfs_node_t *sysfs_ensure_symlink(const char *path, const char *target) {
    return sysfs_ensure_symlink_at(NULL, path, target);
}

vfs_node_t *sysfs_child_append(vfs_node_t *parent, const char *name,
                               bool is_dir) {
    char *path = sysfs_absolute_path(parent, name);
    vfs_node_t *inode;

    if (!path)
        return NULL;
    inode = is_dir ? sysfs_ensure_dir(path) : sysfs_ensure_file(path);
    free(path);
    return inode;
}

vfs_node_t *sysfs_child_append_symlink(vfs_node_t *parent, const char *name,
                                       const char *target_path) {
    char *path = sysfs_absolute_path(parent, name);
    vfs_node_t *inode;

    if (!path)
        return NULL;
    inode = sysfs_ensure_symlink(path, target_path);
    free(path);
    return inode;
}

vfs_node_t *sysfs_regist_dev(char t, int major, int minor,
                             const char *real_device_path, const char *dev_name,
                             const char *other_uevent_content,
                             const char *subsystem_path, const char *class_path,
                             const char *class_name,
                             const char *parent_device_path) {
    const char *root = (t == 'c') ? "char" : "block";
    char dev_root_path[256];
    char devpath_for_event[256];
    char buf[256];
    char *uevent_content = NULL;
    char *event_buffer = NULL;
    vfs_node_t *device_root;

    snprintf(dev_root_path, sizeof(dev_root_path), "/sys/dev/%s/%d:%d", root,
             major, minor);
    if (real_device_path && real_device_path[0]) {
        sysfs_ensure_dir(real_device_path);
        sysfs_ensure_symlink(dev_root_path, real_device_path);
        device_root = sysfs_ensure_dir(real_device_path);
    } else {
        device_root = sysfs_ensure_dir(dev_root_path);
    }
    if (!device_root)
        return NULL;

    snprintf(devpath_for_event, sizeof(devpath_for_event), "%s",
             sysfs_i(device_root)->path + 4);

    vfs_node_t *dev_node = sysfs_child_append(device_root, "dev", false);
    snprintf(buf, sizeof(buf), "%d:%d\n", major, minor);
    sysfs_write_node(dev_node, buf, strlen(buf), 0);
    vfs_iput(dev_node);

    if (subsystem_path && subsystem_path[0]) {
        vfs_node_t *sub = sysfs_child_append_symlink(device_root, "subsystem",
                                                     subsystem_path);
        if (sub)
            vfs_iput(sub);
    }
    if (class_path && class_path[0] && class_name && class_name[0]) {
        char class_link[256];
        snprintf(class_link, sizeof(class_link), "%s/%s", class_path,
                 class_name);
        vfs_node_t *ln =
            sysfs_ensure_symlink(class_link, sysfs_i(device_root)->path);
        if (ln)
            vfs_iput(ln);
    }
    if (parent_device_path && parent_device_path[0]) {
        vfs_node_t *ln = sysfs_child_append_symlink(device_root, "device",
                                                    parent_device_path);
        if (ln)
            vfs_iput(ln);
    }

    vfs_node_t *uevent = sysfs_child_append(device_root, "uevent", false);
    if (!uevent)
        return device_root;

    size_t uevent_content_len =
        snprintf(NULL, 0, "MAJOR=%d\nMINOR=%d\nDEVNAME=%s\nDEVPATH=%s\n%s",
                 major, minor, dev_name, devpath_for_event,
                 other_uevent_content ? other_uevent_content : "") +
        1;
    uevent_content = malloc(uevent_content_len);
    if (!uevent_content) {
        vfs_iput(uevent);
        return device_root;
    }
    snprintf(uevent_content, uevent_content_len,
             "MAJOR=%d\nMINOR=%d\nDEVNAME=%s\nDEVPATH=%s\n%s", major, minor,
             dev_name, devpath_for_event,
             other_uevent_content ? other_uevent_content : "");
    sysfs_write_node(uevent, uevent_content, strlen(uevent_content), 0);
    vfs_iput(uevent);

    int seqnum = alloc_seq_num();
    size_t buffer_len =
        snprintf(NULL, 0, "add@%s\nACTION=add\nSEQNUM=%d\nTAGS=:systemd:\n%s\n",
                 devpath_for_event, seqnum, uevent_content) +
        1;
    event_buffer = malloc(buffer_len);
    if (!event_buffer) {
        free(uevent_content);
        return device_root;
    }
    snprintf(event_buffer, buffer_len,
             "add@%s\nACTION=add\nSEQNUM=%d\nTAGS=:systemd:\n%s\n",
             devpath_for_event, seqnum, uevent_content);
    size_t src_len = strlen(event_buffer);
    size_t dst_len = 0;
    bool last_was_nul = false;
    for (size_t i = 0; i < src_len; i++) {
        char c = event_buffer[i];
        if (c == '\n')
            c = '\0';
        if (c == '\0') {
            if (last_was_nul)
                continue;
            last_was_nul = true;
        } else {
            last_was_nul = false;
        }
        event_buffer[dst_len++] = c;
    }
    if (dst_len == 0 || event_buffer[dst_len - 1] != '\0')
        event_buffer[dst_len++] = '\0';
    netlink_kernel_uevent_send(event_buffer, (int)dst_len);
    free(event_buffer);
    free(uevent_content);

    return device_root;
}

void sysfs_register_device(bus_device_t *device) {
    char bus_root[128];
    char name[128];
    char real_path[256];
    char value_buf[256];
    vfs_node_t *device_root;

    if (!device || !device->bus)
        return;

    snprintf(bus_root, sizeof(bus_root), "/sys/bus/%s", device->bus->name);
    sysfs_ensure_dir(bus_root);
    sysfs_ensure_dir(device->bus->devices_path);
    sysfs_ensure_dir(device->bus->drivers_path);
    device->get_device_path(device, name, sizeof(name));
    snprintf(real_path, sizeof(real_path), "%s/%s", device->bus->devices_path,
             name);

    free(device->sysfs_path);
    free(device->bus_link_path);
    device->sysfs_path = strdup(real_path);
    device->bus_link_path = strdup(real_path);

    device_root = sysfs_ensure_dir(real_path);
    if (!device_root)
        return;

    vfs_node_t *ln =
        sysfs_child_append_symlink(device_root, "subsystem", bus_root);
    if (ln)
        vfs_iput(ln);

    for (int i = 0; i < device->attrs_count; i++) {
        attribute_t *attr = device->attrs[i];
        if (!attr)
            continue;
        vfs_node_t *attr_node =
            sysfs_child_append(device_root, attr->name, false);
        if (attr_node && attr->value) {
            snprintf(value_buf, sizeof(value_buf), "%s\n", attr->value);
            sysfs_write_node(attr_node, value_buf, strlen(value_buf), 0);
        }
        if (attr_node)
            vfs_iput(attr_node);
    }

    for (int i = 0; i < device->bin_attrs_count; i++) {
        bin_attribute_t *bin_attr = device->bin_attrs[i];
        vfs_node_t *attr_node =
            sysfs_child_append(device_root, bin_attr->name, false);
        sysfs_entry_t *entry = sysfs_entry_from_inode(attr_node);
        if (entry) {
            entry->bin_device = device;
            entry->bin_attr = bin_attr;
        }
        if (attr_node)
            vfs_iput(attr_node);
    }

    vfs_iput(device_root);
}

void sysfs_unregister_device(bus_device_t *device) { (void)device; }

void sysfs_init(void) {
    vfs_register_filesystem(&sysfs_fs_type);
    sysfs_ensure_internal_mount();
    vfs_mkdirat(AT_FDCWD, "/sys", 0755, true);
    vfs_do_mount(AT_FDCWD, "/sys", "sysfs", 0, NULL, NULL);

    sysfs_ensure_dir("/sys/fs/cgroup");
    sysfs_ensure_dir("/sys/fs/fuse/connections");
    sysfs_ensure_dir("/sys/kernel/debug");
    sysfs_ensure_dir("/sys/kernel/tracing");
    sysfs_ensure_dir("/sys/devices");
    sysfs_ensure_dir("/sys/devices/usb");
    sysfs_ensure_dir("/sys/module");
    sysfs_ensure_dir("/sys/dev");
    sysfs_ensure_dir("/sys/dev/char");
    sysfs_ensure_dir("/sys/dev/block");
    sysfs_ensure_dir("/sys/bus");
    sysfs_ensure_dir("/sys/bus/pci");
    sysfs_ensure_dir("/sys/bus/pci/devices");
    sysfs_ensure_dir("/sys/bus/pci/drivers");
    sysfs_ensure_dir("/sys/bus/usb");
    sysfs_ensure_dir("/sys/bus/usb/devices");
    sysfs_ensure_dir("/sys/bus/usb/drivers");
    sysfs_ensure_dir("/sys/class");
    sysfs_ensure_dir("/sys/class/graphics");
    sysfs_ensure_dir("/sys/class/input");
    sysfs_ensure_dir("/sys/class/drm");
    sysfs_ensure_dir("/sys/class/net");
    sysfs_ensure_dir("/sys/class/tty");
}

void sysfs_init_umount(void) {
    vfs_do_umount(AT_FDCWD, "/sys", 0);
    if (sysfs_root) {
        vfs_iput(sysfs_root);
        sysfs_root = NULL;
    }
}
