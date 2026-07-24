#include <arch/arch.h>
#include <boot/boot.h>
#include <net/net_syscall.h>
#include <arch/arch.h>
#include <drivers/logger.h>
#include <fs/fs_syscall.h>
#include <fs/vfs/vfs.h>
#include <fs/proc.h>
#include <task/task.h>
#include <net/netlink.h>
#include <net/netdev.h>
#include <libs/hashmap.h>
#include <libs/strerror.h>
#include <init/callbacks.h>

extern socket_op_t socket_ops;

typedef struct sockfs_info {
    spinlock_t lock;
    ino64_t next_ino;
} sockfs_info_t;

typedef struct sockfs_inode_info {
    struct vfs_inode vfs_inode;
} sockfs_inode_info_t;

static struct vfs_file_system_type sockfs_fs_type;
static const struct vfs_super_operations sockfs_super_ops;
static const struct vfs_file_operations sockfs_dir_file_ops;
static const struct vfs_file_operations sockfs_file_ops;
static spinlock_t sockfs_mount_lock;
static struct vfs_mount *sockfs_internal_mnt;

socket_t first_unix_socket;
static socket_t *unix_socket_list_tail = &first_unix_socket;
spinlock_t unix_socket_list_lock;
static spinlock_t unix_socket_bind_lock;

static hashmap_t unix_socket_bind_map = HASHMAP_INIT;

typedef struct unix_socket_bind_bucket {
    uint64_t hash;
    socket_t *head;
} unix_socket_bind_bucket_t;

#define SIOCGIFINDEX 0x8933
#define SIOCGIFNAME 0x8910
#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define SIOCGIFHWADDR 0x8927
#define SIOCGIFMTU 0x8921
#define SIOCGSKNS 0x894C

typedef struct naos_sockaddr {
    uint16_t sa_family;
    char sa_data[14];
} naos_sockaddr_t;

typedef struct naos_ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        naos_sockaddr_t ifru_hwaddr;
        short ifru_flags;
        int ifru_ifindex;
        int ifru_metric;
        int ifru_mtu;
        char ifru_slave[IFNAMSIZ];
        char ifru_newname[IFNAMSIZ];
        char ifru_pad[24];
        void *ifru_data;
    } ifr_ifru;
} naos_ifreq_t;

static uint16_t socket_netdev_flags(const netdev_t *dev) {
    uint32_t flags = IFF_BROADCAST | IFF_MULTICAST;

    if (!dev)
        return (uint16_t)flags;
    if (dev->admin_up)
        flags |= IFF_UP;
    if (dev->link_up)
        flags |= IFF_RUNNING | IFF_LOWER_UP;
    return (uint16_t)flags;
}

static inline void socket_notify_sock(socket_t *sock, uint32_t events);
static inline void unix_socket_snapshot_peer_cred(socket_t *sock,
                                                  const struct ucred *cred);
static inline void unix_socket_release_lookup_ref(socket_t *sock);
void unix_socket_free(socket_t *sock);
static void unix_socket_handle_release(socket_handle_t *handle);
int socket_poll(fd_t *fd, size_t events);
int socket_ioctl(fd_t *fd, ssize_t cmd, ssize_t arg);
ssize_t socket_read(fd_t *fd, void *buf, size_t offset, size_t limit);
ssize_t socket_write(fd_t *fd, const void *buf, size_t offset, size_t limit);

static inline void unix_socket_fill_timestamp_now(struct timeval *tv) {
    uint64_t now_ns;

    if (!tv)
        return;

    now_ns = nano_time();
    tv->tv_sec = boot_get_boottime() + now_ns / 1000000000ULL;
    tv->tv_usec = (now_ns % 1000000000ULL) / 1000ULL;
}

static inline sockfs_info_t *sockfs_sb_info(struct vfs_super_block *sb) {
    return sb ? (sockfs_info_t *)sb->s_fs_info : NULL;
}

socket_handle_t *sockfs_inode_socket_handle(vfs_node_t *node) {
    return node ? (socket_handle_t *)node->i_private : NULL;
}

socket_handle_t *sockfs_file_handle(fd_t *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (socket_handle_t *)file->private_data;
    return sockfs_inode_socket_handle(file->f_inode);
}

static inline socket_t *socket_file_sock(fd_t *file) {
    socket_handle_t *handle = sockfs_file_handle(file);
    return handle ? (socket_t *)handle->sock : NULL;
}

static struct vfs_inode *sockfs_alloc_inode(struct vfs_super_block *sb) {
    sockfs_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void sockfs_destroy_inode(struct vfs_inode *inode) {
    socket_handle_t *handle;

    if (!inode)
        return;

    free(container_of(inode, sockfs_inode_info_t, vfs_inode));
}

static void sockfs_put_super(struct vfs_super_block *sb) {
    if (sb && sb->s_fs_info)
        free(sb->s_fs_info);
}

static int sockfs_statfs(struct vfs_path *path, void *buf) {
    struct statfs *st = (struct statfs *)buf;
    struct vfs_super_block *sb;

    if (!path || !path->dentry || !path->dentry->d_inode || !st)
        return -EINVAL;

    memset(st, 0, sizeof(*st));
    sb = path->dentry->d_inode->i_sb;
    if (!sb)
        return 0;

    st->f_type = sb->s_magic;
    st->f_bsize = PAGE_SIZE;
    st->f_frsize = PAGE_SIZE;
    st->f_namelen = 255;
    st->f_flags = sb->s_flags;
    return 0;
}

static int sockfs_init_fs_context(struct vfs_fs_context *fc) {
    (void)fc;
    return 0;
}

static int sockfs_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    sockfs_info_t *fsi;
    struct vfs_inode *inode;
    struct vfs_dentry *root;

    if (!fc)
        return -EINVAL;

    sb = vfs_alloc_super(fc->fs_type, fc->sb_flags);
    if (!sb)
        return -ENOMEM;

    fsi = calloc(1, sizeof(*fsi));
    if (!fsi) {
        vfs_put_super(sb);
        return -ENOMEM;
    }

    spin_init(&fsi->lock);
    fsi->next_ino = 1;
    sb->s_magic = 0x736f636bULL;
    sb->s_fs_info = fsi;
    sb->s_op = &sockfs_super_ops;
    sb->s_type = &sockfs_fs_type;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        free(fsi);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    inode->i_ino = 1;
    inode->inode = 1;
    inode->i_mode = S_IFDIR | 0700;
    inode->i_nlink = 2;
    inode->i_fop = &sockfs_dir_file_ops;

    root = vfs_d_alloc(sb, NULL, NULL);
    if (!root) {
        vfs_iput(inode);
        free(fsi);
        vfs_put_super(sb);
        return -ENOMEM;
    }

    vfs_d_instantiate(root, inode);
    sb->s_root = root;
    fc->sb = sb;
    return 0;
}

static const struct vfs_super_operations sockfs_super_ops = {
    .alloc_inode = sockfs_alloc_inode,
    .destroy_inode = sockfs_destroy_inode,
    .put_super = sockfs_put_super,
    .statfs = sockfs_statfs,
};

static struct vfs_file_system_type sockfs_fs_type = {
    .name = "sockfs",
    .fs_flags = VFS_FS_VIRTUAL,
    .init_fs_context = sockfs_init_fs_context,
    .get_tree = sockfs_get_tree,
};

static struct vfs_mount *sockfs_get_internal_mount(void) {
    int ret;

    spin_lock(&sockfs_mount_lock);
    if (!sockfs_internal_mnt) {
        ret = vfs_kern_mount("sockfs", 0, NULL, NULL, &sockfs_internal_mnt);
        if (ret < 0)
            sockfs_internal_mnt = NULL;
    }
    if (sockfs_internal_mnt)
        vfs_mntget(sockfs_internal_mnt);
    spin_unlock(&sockfs_mount_lock);
    return sockfs_internal_mnt;
}

static ino64_t sockfs_next_ino(struct vfs_super_block *sb) {
    sockfs_info_t *fsi = sockfs_sb_info(sb);
    ino64_t ino;

    spin_lock(&fsi->lock);
    ino = ++fsi->next_ino;
    spin_unlock(&fsi->lock);
    return ino;
}

static loff_t sockfs_llseek(struct vfs_file *file, loff_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static int sockfs_open(struct vfs_inode *inode, struct vfs_file *file) {
    if (!file)
        return -EINVAL;
    file->private_data = inode ? inode->i_private : NULL;
    return 0;
}

static int sockfs_release(struct vfs_inode *inode, struct vfs_file *file) {
    socket_handle_t *handle;

    if (!file)
        return 0;

    handle = sockfs_file_handle(file);
    file->private_data = NULL;
    if (inode && inode->i_private == handle)
        inode->i_private = NULL;

    if (handle) {
        if (handle->release)
            handle->release(handle);
        else
            free(handle);
    }
    return 0;
}

static inline bool unix_socket_is_dgram_type(int type) {
    return type == SOCK_DGRAM;
}

static inline bool unix_socket_is_message_type(int type) {
    return type == SOCK_DGRAM || type == SOCK_SEQPACKET;
}

static inline bool unix_socket_is_connected_type(int type) {
    return type == SOCK_STREAM || type == SOCK_SEQPACKET;
}

static inline bool unix_socket_type_supported(int type) {
    return unix_socket_is_connected_type(type) ||
           unix_socket_is_dgram_type(type);
}

static inline socket_t *unix_socket_get(socket_t *sock) {
    if (!sock)
        return NULL;

    __atomic_add_fetch(&sock->refs, 1, __ATOMIC_ACQ_REL);
    return sock;
}

static socket_t *unix_socket_get_peer_ref(socket_t *sock) {
    socket_t *peer = NULL;

    if (!sock)
        return NULL;

    spin_lock(&sock->lock);
    peer = sock->peer;
    if (peer)
        unix_socket_get(peer);
    spin_unlock(&sock->lock);
    return peer;
}

static inline void unix_socket_put_peer_ref(socket_t *peer, bool lookup_ref) {
    if (!peer)
        return;

    if (lookup_ref)
        unix_socket_release_lookup_ref(peer);
    else
        unix_socket_free(peer);
}

static inline void unix_socket_set_peer(socket_t *sock, socket_t *peer) {
    if (!sock)
        return;

    if (peer)
        unix_socket_get(peer);
    sock->peer = peer;
    sock->peer_ref = peer != NULL;
}

static inline uint32_t socket_expand_events(uint32_t events) {
    if (events & EPOLLIN)
        events |= EPOLLRDNORM;
    if (events & EPOLLRDNORM)
        events |= EPOLLIN;
    if (events & EPOLLOUT)
        events |= EPOLLWRNORM;
    if (events & EPOLLWRNORM)
        events |= EPOLLOUT;
    return events;
}

static inline bool socket_wants_input(uint32_t events) {
    return socket_expand_events(events) & EPOLLIN;
}

static inline bool socket_wants_output(uint32_t events) {
    return socket_expand_events(events) & EPOLLOUT;
}

static inline uint32_t socket_ready_input(uint32_t events) {
    return socket_wants_input(events) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static inline uint32_t socket_ready_output(uint32_t events) {
    return socket_wants_output(events) ? (EPOLLOUT | EPOLLWRNORM) : 0;
}

static inline int32_t unix_socket_cred_pid_for_task(task_t *task) {
    if (!task)
        return -1;

    uint64_t pid = task_effective_tgid(task);
    if (pid > INT32_MAX)
        return -1;

    return (int32_t)pid;
}

static inline void unix_socket_fill_cred_from_task(struct ucred *cred,
                                                   task_t *task) {
    if (!cred) {
        return;
    }

    cred->pid = unix_socket_cred_pid_for_task(task);
    cred->uid = task ? task->uid : 0;
    cred->gid = task ? task->gid : 0;
}

static inline void unix_socket_snapshot_peer_cred(socket_t *sock,
                                                  const struct ucred *cred) {
    if (!sock || !cred) {
        return;
    }

    sock->peer_cred = *cred;
    sock->has_peer_cred = true;
}

static inline bool unix_socket_get_peer_cred(const socket_t *sock,
                                             struct ucred *cred) {
    if (!sock || !cred) {
        return false;
    }

    if (sock->has_peer_cred) {
        *cred = sock->peer_cred;
        return true;
    }

    if (!sock->peer) {
        return false;
    }

    *cred = sock->peer->cred;
    return true;
}

static int unix_socket_maybe_add_passcred(socket_t *peer,
                                          unix_socket_ancillary_t **ancillary) {
    bool peer_passcred = false;

    if (!peer || !ancillary)
        return 0;

    spin_lock(&peer->lock);
    peer_passcred = peer->passcred;
    spin_unlock(&peer->lock);

    if (!peer_passcred)
        return 0;

    if (!*ancillary) {
        *ancillary = calloc(1, sizeof(**ancillary));
        if (!*ancillary)
            return -ENOMEM;
    }

    if (!(*ancillary)->has_cred) {
        unix_socket_fill_cred_from_task(&(*ancillary)->cred, current_task);
        (*ancillary)->has_cred = true;
    }

    return 0;
}

static int unix_socket_ensure_sender_cred(unix_socket_ancillary_t **ancillary,
                                          bool explicit_cred) {
    if (!ancillary)
        return 0;

    if (!*ancillary) {
        *ancillary = calloc(1, sizeof(**ancillary));
        if (!*ancillary)
            return -ENOMEM;
    }

    if (!(*ancillary)->has_cred) {
        unix_socket_fill_cred_from_task(&(*ancillary)->cred, current_task);
        (*ancillary)->has_cred = true;
    }
    if (explicit_cred)
        (*ancillary)->cred_explicit = true;

    return 0;
}

static int
unix_socket_maybe_add_timestamp(socket_t *peer,
                                unix_socket_ancillary_t **ancillary) {
    bool peer_timestamp = false;

    if (!peer || !ancillary)
        return 0;

    spin_lock(&peer->lock);
    peer_timestamp = peer->timestamp_legacy != 0;
    spin_unlock(&peer->lock);

    if (!peer_timestamp)
        return 0;

    if (!*ancillary) {
        *ancillary = calloc(1, sizeof(**ancillary));
        if (!*ancillary)
            return -ENOMEM;
    }

    if (!(*ancillary)->has_timestamp) {
        unix_socket_fill_timestamp_now(&(*ancillary)->timestamp);
        (*ancillary)->has_timestamp = true;
    }

    return 0;
}

static uint64_t unix_socket_name_hash(const char *name) {
    uint64_t hash = 1469598103934665603ULL;
    if (!name)
        return hash;

    while (*name) {
        hash ^= (uint8_t)*name++;
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void unix_socket_unlink_bound_path(const char *path) {
    if (!path || !path[0])
        return;
    (void)vfs_unlinkat(AT_FDCWD, path, 0, true);
}

static inline unix_socket_bind_bucket_t *
unix_socket_bind_bucket_lookup_locked(uint64_t hash) {
    return (unix_socket_bind_bucket_t *)hashmap_get(&unix_socket_bind_map,
                                                    hash);
}

static socket_t *unix_socket_lookup_bound_locked(const char *name, size_t len,
                                                 socket_t *skip,
                                                 bool take_node_ref) {
    if (!name || !len)
        return NULL;

    uint64_t hash = unix_socket_name_hash(name);
    unix_socket_bind_bucket_t *bucket =
        unix_socket_bind_bucket_lookup_locked(hash);
    socket_t *sock = bucket ? bucket->head : NULL;
    while (sock) {
        if (sock != skip && sock->bindHash == hash && sock->bindAddr &&
            sock->bindAddrLen == len &&
            memcmp(sock->bindAddr, name, len) == 0) {
            if (take_node_ref) {
                if (!sock->node || !vfs_igrab(sock->node))
                    return NULL;
                unix_socket_get(sock);
            }
            return sock;
        }
        sock = sock->bind_next;
    }

    return NULL;
}

static socket_t *unix_socket_lookup_bound(const char *name, size_t len,
                                          socket_t *skip, bool take_node_ref) {
    socket_t *sock = NULL;

    spin_lock(&unix_socket_bind_lock);
    sock = unix_socket_lookup_bound_locked(name, len, skip, take_node_ref);
    spin_unlock(&unix_socket_bind_lock);

    return sock;
}

static void unix_socket_unbind(socket_t *sock) {
    if (!sock || !sock->bindAddr)
        return;

    spin_lock(&unix_socket_bind_lock);
    unix_socket_bind_bucket_t *bucket =
        unix_socket_bind_bucket_lookup_locked(sock->bindHash);
    socket_t *bind_head = bucket ? bucket->head : NULL;
    socket_t *prev = NULL;
    socket_t *curr = bind_head;
    while (curr && curr != sock) {
        prev = curr;
        curr = curr->bind_next;
    }
    if (curr == sock) {
        if (prev) {
            prev->bind_next = curr->bind_next;
        } else {
            if (bucket)
                bucket->head = curr->bind_next;
            if (!bucket || !bucket->head) {
                hashmap_remove(&unix_socket_bind_map, sock->bindHash);
                free(bucket);
            }
        }
        curr->bind_next = NULL;
    }
    spin_unlock(&unix_socket_bind_lock);

    if (sock->bindAddr[0] != '@')
        unix_socket_unlink_bound_path(sock->bindAddr);
    free(sock->bindAddr);
    sock->bindAddr = NULL;
    sock->bindAddrLen = 0;
    sock->bindHash = 0;
}

static inline void unix_socket_release_lookup_ref(socket_t *sock) {
    if (sock && sock->node)
        vfs_iput(sock->node);
    unix_socket_free(sock);
}

static int unix_socket_missing_peer_error(const char *safe, bool is_dgram) {
    if (safe && safe[0] && safe[0] != '@') {
        struct vfs_path path_node = {0};
        if (vfs_filename_lookup(AT_FDCWD, safe, LOOKUP_FOLLOW, &path_node) ==
            0) {
            vfs_path_put(&path_node);
            return -ECONNREFUSED;
        }
    }

    return is_dgram ? -EDESTADDRREQ : -ENOTCONN;
}

char *unix_socket_addr_safe(const struct sockaddr_un *addr, size_t len) {
    ssize_t addrLen = len - sizeof(addr->sun_family);
    if (addrLen <= 0)
        return (void *)-EINVAL;

    bool abstract = (addr->sun_path[0] == '\0');
    int skip = abstract ? 1 : 0;

    char *safe = malloc(addrLen + 3);
    if (!safe)
        return (void *)-(ENOMEM);
    memset(safe, 0, addrLen + 3);

    if (abstract && addr->sun_path[1] == '\0') {
        free(safe);
        return (char *)-EINVAL;
    }

    if (abstract) {
        safe[0] = '@';
        memcpy(safe + 1, addr->sun_path + skip, addrLen - skip);
    } else {
        memcpy(safe, addr->sun_path, addrLen);
    }

    return safe;
}

static inline void socket_pending_mark(socket_t *sock, uint32_t events) {
    if (!sock || !events)
        return;
    events = socket_expand_events(events);
    __atomic_fetch_or(&sock->pending_events, events, __ATOMIC_RELEASE);
}

static inline uint32_t socket_pending_take(socket_t *sock, uint32_t events) {
    if (!sock || !events)
        return 0;

    events = socket_expand_events(events);

    uint32_t old_mask = 0;
    uint32_t new_mask = 0;
    do {
        old_mask = __atomic_load_n(&sock->pending_events, __ATOMIC_ACQUIRE);
        if (!(old_mask & events))
            return 0;
        new_mask = old_mask & ~events;
    } while (!__atomic_compare_exchange_n(&sock->pending_events, &old_mask,
                                          new_mask, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));

    return old_mask & events;
}

static inline void socket_notify_sock(socket_t *sock, uint32_t events) {
    if (!sock)
        return;
    events = socket_expand_events(events);
    socket_pending_mark(sock, events);
    if (sock->node)
        vfs_poll_notify_inode(sock->node, events);
}

static inline void unix_socket_lock_pair(socket_t *a, socket_t *b) {
    if (!a)
        return;

    if (!b || a == b) {
        spin_lock(&a->lock);
        return;
    }

    if (a < b) {
        spin_lock(&a->lock);
        spin_lock(&b->lock);
    } else {
        spin_lock(&b->lock);
        spin_lock(&a->lock);
    }
}

static inline void unix_socket_unlock_pair(socket_t *a, socket_t *b) {
    if (!a)
        return;

    if (!b || a == b) {
        spin_unlock(&a->lock);
        return;
    }

    spin_unlock(&a->lock);
    spin_unlock(&b->lock);
}

static void unix_socket_disconnect_peer_links(socket_t *sock, bool mark_closed,
                                              bool notify_self,
                                              bool notify_peer) {
    socket_t *peer = NULL;
    socket_t *drop_sock_peer = NULL;
    socket_t *drop_peer_sock = NULL;

    if (!sock)
        return;

    peer = sock->peer;
    unix_socket_lock_pair(sock, peer);

    if (mark_closed)
        sock->closed = true;

    if (peer && sock->peer == peer) {
        unix_socket_snapshot_peer_cred(sock, &peer->cred);
        if (sock->peer_ref)
            drop_sock_peer = peer;
        sock->peer = NULL;
        sock->peer_ref = false;
    }

    if (peer && peer->peer == sock) {
        unix_socket_snapshot_peer_cred(peer, &sock->cred);
        if (peer->peer_ref)
            drop_peer_sock = sock;
        peer->peer = NULL;
        peer->peer_ref = false;
    }

    unix_socket_unlock_pair(sock, peer);

    if (notify_self)
        socket_notify_sock(sock, EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR);
    if (notify_peer && peer)
        socket_notify_sock(peer, EPOLLIN | EPOLLHUP | EPOLLRDHUP);

    unix_socket_free(drop_sock_peer);
    unix_socket_free(drop_peer_sock);
}

static void unix_socket_close_owned(socket_t *sock) {
    if (!sock)
        return;

    unix_socket_disconnect_peer_links(sock, true, true, true);
    unix_socket_unbind(sock);
    unix_socket_free(sock);
}

static bool unix_socket_backlog_reserve_locked(socket_t *sock,
                                               int min_capacity) {
    if (!sock || min_capacity <= 0)
        return true;

    if (sock->backlogCap >= min_capacity)
        return true;

    if (sock->connMax <= 0 || min_capacity > sock->connMax)
        return false;

    int new_capacity = sock->backlogCap;
    if (new_capacity <= 0)
        new_capacity = MIN(sock->connMax, 16);

    while (new_capacity < min_capacity) {
        if (new_capacity >= sock->connMax) {
            new_capacity = sock->connMax;
            break;
        }

        if (new_capacity > sock->connMax / 2) {
            new_capacity = sock->connMax;
        } else {
            new_capacity *= 2;
        }
    }

    if (new_capacity < min_capacity)
        return false;

    socket_t **new_backlog = calloc((size_t)new_capacity, sizeof(*new_backlog));
    if (!new_backlog)
        return false;

    for (int i = 0; i < sock->connCurr; i++) {
        int slot = (sock->connHead + i) % sock->backlogCap;
        new_backlog[i] = sock->backlog[slot];
    }

    free(sock->backlog);
    sock->backlog = new_backlog;
    sock->backlogCap = new_capacity;
    sock->connHead = 0;
    return true;
}

static inline size_t unix_socket_recv_used_locked(const socket_t *sock) {
    return sock ? skb_queue_bytes(&sock->recv_queue) : 0;
}

static inline size_t unix_socket_recv_space_locked(const socket_t *sock) {
    return sock ? skb_queue_space(&sock->recv_queue) : 0;
}

static inline size_t unix_socket_next_packet_len_locked(const socket_t *sock) {
    skb_buff_t *skb = NULL;

    if (!sock)
        return 0;

    skb = skb_queue_peek(&sock->recv_queue);
    return skb ? skb_unread_len(skb) : 0;
}

static void unix_socket_ancillary_free(unix_socket_ancillary_t *ancillary) {
    if (!ancillary)
        return;

    for (uint32_t i = 0; i < ancillary->file_count; i++) {
        if (ancillary->files[i])
            vfs_file_put(ancillary->files[i]);
    }

    free(ancillary);
}

static void
unix_socket_ancillary_free_list(unix_socket_ancillary_t *ancillary_list) {
    while (ancillary_list) {
        unix_socket_ancillary_t *next = ancillary_list->next;
        unix_socket_ancillary_free(ancillary_list);
        ancillary_list = next;
    }
}

static int unix_socket_ancillary_append(unix_socket_ancillary_t **head,
                                        unix_socket_ancillary_t **tail,
                                        unix_socket_ancillary_t *ancillary) {
    if (!ancillary)
        return 0;

    ancillary->next = NULL;
    if (*tail) {
        (*tail)->next = ancillary;
    } else {
        *head = ancillary;
    }
    *tail = ancillary;
    return 0;
}

static unix_socket_ancillary_t *
unix_socket_ancillary_clone_one(const unix_socket_ancillary_t *src) {
    if (!src)
        return NULL;

    unix_socket_ancillary_t *clone = calloc(1, sizeof(*clone));
    if (!clone)
        return NULL;

    clone->file_count = src->file_count;
    clone->cred = src->cred;
    clone->has_cred = src->has_cred;
    clone->cred_explicit = src->cred_explicit;
    clone->timestamp = src->timestamp;
    clone->has_timestamp = src->has_timestamp;

    for (uint32_t i = 0; i < src->file_count; i++) {
        clone->files[i] = vfs_file_get(src->files[i]);
        if (!clone->files[i]) {
            unix_socket_ancillary_free(clone);
            return NULL;
        }
    }

    return clone;
}

static int unix_socket_collect_skb_ancillary(skb_buff_t *skb, bool peek,
                                             unix_socket_ancillary_t **head,
                                             unix_socket_ancillary_t **tail) {
    unix_socket_ancillary_t *ancillary = NULL;

    if (!skb || skb->offset != 0 || !skb->priv)
        return 0;

    if (peek) {
        ancillary = unix_socket_ancillary_clone_one(
            (unix_socket_ancillary_t *)skb->priv);
        if (!ancillary)
            return -ENOMEM;
    } else {
        ancillary = (unix_socket_ancillary_t *)skb_detach_priv(skb);
    }

    return unix_socket_ancillary_append(head, tail, ancillary);
}

static void unix_socket_drop_skb_ancillary(skb_buff_t *skb) {
    unix_socket_ancillary_t *ancillary = NULL;

    if (!skb || skb->offset != 0 || !skb->priv)
        return;

    ancillary = (unix_socket_ancillary_t *)skb_detach_priv(skb);
    unix_socket_ancillary_free(ancillary);
}

static bool unix_socket_skb_has_rights(const skb_buff_t *skb) {
    unix_socket_ancillary_t *ancillary = NULL;

    if (!skb || !skb->priv)
        return false;

    ancillary = (unix_socket_ancillary_t *)skb->priv;
    return ancillary->file_count > 0;
}

static int unix_socket_iov_total_len(const struct iovec *iov, size_t iovlen,
                                     size_t *out_total) {
    size_t total = 0;

    if (!out_total)
        return -EINVAL;

    *out_total = 0;
    if (iovlen && !iov)
        return -EFAULT;

    for (size_t i = 0; i < iovlen; i++) {
        if (iov[i].len > SIZE_MAX - total)
            return -EMSGSIZE;
        total += iov[i].len;
    }

    *out_total = total;
    return 0;
}

static size_t unix_socket_iov_copy_out(const struct iovec *iov, size_t iovlen,
                                       size_t dest_offset, const uint8_t *src,
                                       size_t len) {
    size_t copied = 0;
    size_t skipped = 0;

    if (!iov || !src || !len)
        return 0;

    for (size_t i = 0; i < iovlen && copied < len; i++) {
        if (!iov[i].iov_base || !iov[i].len)
            continue;

        if (dest_offset >= skipped + iov[i].len) {
            skipped += iov[i].len;
            continue;
        }

        size_t inner_off = 0;
        if (dest_offset > skipped)
            inner_off = dest_offset - skipped;

        size_t room = iov[i].len - inner_off;
        size_t copy_len = MIN(room, len - copied);
        memcpy((uint8_t *)iov[i].iov_base + inner_off, src + copied, copy_len);
        copied += copy_len;
        skipped += iov[i].len;
    }

    return copied;
}

static skb_buff_t *unix_socket_build_skb(const uint8_t *data, size_t len,
                                         unix_socket_ancillary_t *ancillary) {
    skb_buff_t *skb = skb_alloc(len);

    if (!skb) {
        unix_socket_ancillary_free(ancillary);
        return NULL;
    }

    if (len > 0 && data)
        memcpy(skb->data, data, len);

    skb->priv = ancillary;
    return skb;
}

static skb_buff_t *
unix_socket_build_skb_from_iov(const struct iovec *iov, size_t iovlen,
                               size_t total_len, unix_socket_ancillary_t *anc) {
    skb_buff_t *skb = skb_alloc(total_len);
    size_t copied = 0;

    if (!skb) {
        unix_socket_ancillary_free(anc);
        return NULL;
    }

    for (size_t i = 0; i < iovlen; i++) {
        if (!iov[i].iov_base || !iov[i].len)
            continue;
        if (iov[i].len > total_len - copied) {
            skb_free(skb, NULL);
            unix_socket_ancillary_free(anc);
            return NULL;
        }
        memcpy(skb->data + copied, iov[i].iov_base, iov[i].len);
        copied += iov[i].len;
    }

    skb->priv = anc;
    return skb;
}

static size_t
unix_socket_stream_read_locked(socket_t *sock, uint8_t *out, size_t len,
                               bool peek,
                               unix_socket_ancillary_t **ancillary_head,
                               unix_socket_ancillary_t **ancillary_tail) {
    size_t copied = 0;
    skb_buff_t *skb = NULL;

    /*
     * STREAM is a byte stream. Do not treat skb boundaries in recv_queue as
     * message boundaries; userspace must only observe byte ordering here.
     */
    if (!sock || !len)
        return 0;

    for (skb = skb_queue_peek(&sock->recv_queue); skb && copied < len;) {
        size_t unread = skb_unread_len(skb);
        size_t chunk = MIN(len - copied, unread);
        bool rights_barrier = unix_socket_skb_has_rights(skb);
        if (!chunk)
            break;

        if (out)
            skb_copy_data(skb, 0, out + copied, chunk);

        if (ancillary_head && ancillary_tail) {
            if (unix_socket_collect_skb_ancillary(skb, peek, ancillary_head,
                                                  ancillary_tail) < 0)
                return (size_t)-ENOMEM;
        } else if (!peek) {
            unix_socket_drop_skb_ancillary(skb);
        }

        copied += chunk;

        if (!peek) {
            sock->recv_queue.byte_count -= chunk;
            skb->offset += chunk;
            if (skb_unread_len(skb) == 0) {
                skb_buff_t *done = skb_queue_pop(&sock->recv_queue);
                skb_free(done, NULL);
            }
        }

        /*
         * Linux stops a stream recv at an skb carrying SCM_RIGHTS, so the
         * returned bytes stay aligned with the descriptor array.
         */
        if (rights_barrier)
            break;

        skb = peek ? skb->next : skb_queue_peek(&sock->recv_queue);
    }

    return copied;
}

static size_t
unix_socket_stream_readv_locked(socket_t *sock, const struct iovec *iov,
                                size_t iovlen, size_t len_total, bool peek,
                                unix_socket_ancillary_t **ancillary_head,
                                unix_socket_ancillary_t **ancillary_tail) {
    size_t copied = 0;
    skb_buff_t *skb = NULL;

    if (!sock || !iov || !iovlen || !len_total)
        return 0;

    for (skb = skb_queue_peek(&sock->recv_queue); skb && copied < len_total;) {
        size_t unread = skb_unread_len(skb);
        size_t chunk = MIN(len_total - copied, unread);
        bool rights_barrier = unix_socket_skb_has_rights(skb);
        if (!chunk)
            break;

        if (iov)
            unix_socket_iov_copy_out(iov, iovlen, copied,
                                     skb->data + skb->offset, chunk);

        if (ancillary_head && ancillary_tail) {
            if (unix_socket_collect_skb_ancillary(skb, peek, ancillary_head,
                                                  ancillary_tail) < 0)
                return (size_t)-ENOMEM;
        } else if (!peek) {
            unix_socket_drop_skb_ancillary(skb);
        }

        copied += chunk;

        if (!peek) {
            sock->recv_queue.byte_count -= chunk;
            skb->offset += chunk;
            if (skb_unread_len(skb) == 0) {
                skb_buff_t *done = skb_queue_pop(&sock->recv_queue);
                skb_free(done, NULL);
            }
        }

        /*
         * Linux stops a stream recv at an skb carrying SCM_RIGHTS, so the
         * returned bytes stay aligned with the descriptor array.
         */
        if (rights_barrier)
            break;

        skb = peek ? skb->next : skb_queue_peek(&sock->recv_queue);
    }

    return copied;
}

static size_t unix_socket_packet_readv_locked(
    socket_t *sock, const struct iovec *iov, size_t iovlen, size_t len_total,
    bool peek, bool *truncated, unix_socket_ancillary_t **ancillary_head,
    unix_socket_ancillary_t **ancillary_tail) {
    skb_buff_t *skb = NULL;
    size_t packet_len = 0;
    size_t copied = 0;

    /*
     * DGRAM and SEQPACKET keep packet boundaries. One read consumes the head
     * packet only; short buffers truncate the payload and report MSG_TRUNC.
     */
    if (!sock || !iov || !iovlen)
        return 0;

    skb = skb_queue_peek(&sock->recv_queue);
    if (!skb)
        return 0;

    packet_len = skb_unread_len(skb);
    copied = MIN(len_total, packet_len);
    if (truncated)
        *truncated = packet_len > copied;

    if (copied > 0)
        unix_socket_iov_copy_out(iov, iovlen, 0, skb->data + skb->offset,
                                 copied);

    if (ancillary_head && ancillary_tail) {
        if (unix_socket_collect_skb_ancillary(skb, peek, ancillary_head,
                                              ancillary_tail) < 0)
            return (size_t)-ENOMEM;
    } else if (!peek) {
        unix_socket_drop_skb_ancillary(skb);
    }

    if (!peek) {
        skb_buff_t *done = skb_queue_pop(&sock->recv_queue);
        skb_free(done, NULL);
    }

    return copied;
}

static size_t
unix_socket_packet_read_locked(socket_t *sock, uint8_t *out, size_t len,
                               bool peek, bool *truncated,
                               unix_socket_ancillary_t **ancillary_head,
                               unix_socket_ancillary_t **ancillary_tail) {
    skb_buff_t *skb = NULL;
    size_t packet_len = 0;
    size_t copied = 0;

    if (!sock)
        return 0;

    skb = skb_queue_peek(&sock->recv_queue);
    if (!skb)
        return 0;

    packet_len = skb_unread_len(skb);
    copied = MIN(len, packet_len);
    if (truncated)
        *truncated = packet_len > copied;

    if (copied > 0 && out)
        skb_copy_data(skb, 0, out, copied);

    if (ancillary_head && ancillary_tail) {
        if (unix_socket_collect_skb_ancillary(skb, peek, ancillary_head,
                                              ancillary_tail) < 0)
            return (size_t)-ENOMEM;
    } else if (!peek) {
        unix_socket_drop_skb_ancillary(skb);
    }

    if (!peek) {
        skb_buff_t *done = skb_queue_pop(&sock->recv_queue);
        skb_free(done, NULL);
    }

    return copied;
}

static int unix_socket_prepare_ancillary(const struct msghdr *msg,
                                         unix_socket_ancillary_t **out_anc) {
    if (!out_anc)
        return -EINVAL;

    *out_anc = NULL;
    if (!msg || !msg->msg_control || msg->msg_controllen == 0)
        return 0;

    unix_socket_ancillary_t *anc = calloc(1, sizeof(*anc));
    if (!anc)
        return -ENOMEM;

    bool have_rights = false;
    bool have_cred = false;
    uint8_t *control = (uint8_t *)msg->msg_control;
    size_t offset = 0;

    while (offset + sizeof(struct cmsghdr) <= msg->msg_controllen) {
        struct cmsghdr *cmsg = (struct cmsghdr *)(control + offset);
        size_t cmsg_len = cmsg->cmsg_len;
        size_t aligned_len;

        if (cmsg_len < sizeof(struct cmsghdr) ||
            cmsg_len > msg->msg_controllen - offset) {
            unix_socket_ancillary_free(anc);
            return -EINVAL;
        }
        if (cmsg_len > SIZE_MAX - sizeof(size_t) + 1) {
            unix_socket_ancillary_free(anc);
            return -EINVAL;
        }
        aligned_len = CMSG_ALIGN(cmsg_len);
        if (aligned_len == 0 || aligned_len > msg->msg_controllen - offset)
            offset = msg->msg_controllen;
        else
            offset += aligned_len;

        if (cmsg->cmsg_level != SOL_SOCKET)
            continue;

        if (cmsg->cmsg_type == SCM_RIGHTS) {
            if (have_rights || cmsg->cmsg_len < CMSG_LEN(0)) {
                unix_socket_ancillary_free(anc);
                return -EINVAL;
            }

            size_t rights_len = cmsg->cmsg_len - CMSG_LEN(0);
            if ((rights_len % sizeof(int)) != 0) {
                unix_socket_ancillary_free(anc);
                return -EINVAL;
            }

            uint32_t file_count = rights_len / sizeof(int);
            /*
             * Linux accepts an empty SCM_RIGHTS header and effectively treats
             * it as "no descriptors attached". Firefox's MiniTransceiver uses
             * exactly that encoding for ordinary messages that carry no FDs.
             */
            if (file_count == 0) {
                continue;
            }

            if (file_count > MAX_PENDING_FILES_COUNT) {
                unix_socket_ancillary_free(anc);
                return -ETOOMANYREFS;
            }

            int *fds = (int *)CMSG_DATA(cmsg);
            for (uint32_t i = 0; i < file_count; i++) {
                int send_fd = fds[i];
                fd_t *send_file = task_get_file(current_task, send_fd);
                if (send_fd < 0 || !send_file) {
                    unix_socket_ancillary_free(anc);
                    return -EBADF;
                }

                anc->files[anc->file_count] = send_file;
                anc->file_count++;
            }

            have_rights = true;
        } else if (cmsg->cmsg_type == SCM_CREDENTIALS) {
            if (have_cred || cmsg->cmsg_len < CMSG_LEN(sizeof(struct ucred))) {
                unix_socket_ancillary_free(anc);
                return -EINVAL;
            }

            struct ucred *cred = (struct ucred *)CMSG_DATA(cmsg);
            if (current_task->euid != 0 &&
                (cred->pid != unix_socket_cred_pid_for_task(current_task) ||
                 cred->uid != current_task->uid ||
                 cred->gid != current_task->gid)) {
                unix_socket_ancillary_free(anc);
                return -EPERM;
            }

            anc->cred = *cred;
            anc->has_cred = true;
            anc->cred_explicit = true;
            have_cred = true;
        }
    }

    if (!anc->file_count && !anc->has_cred) {
        free(anc);
        return 0;
    }

    *out_anc = anc;
    return 0;
}

static int socket_wait_file(fd_t *file, uint32_t events, const char *reason) {
    if (!file || !file->node || !current_task)
        return -EINVAL;

    socket_t *wait_sock = socket_file_sock(file);
    const uint32_t want = events | EPOLLERR | EPOLLHUP | EPOLLNVAL | EPOLLRDHUP;

    uint32_t polled;
    while (true) {
        if (socket_pending_take(wait_sock, want))
            return EOK;

        int poll_ret = vfs_poll(file, want);
        if (poll_ret < 0)
            return poll_ret;
        polled = (uint32_t)poll_ret;

        if (polled & want)
            return EOK;

        int reason = vfs_poll_wait_interruptible(file, want);
        if (reason < 0)
            return reason;
    }
}

static int socket_wait_listen_backlog(socket_t *listen_sock) {
    if (!listen_sock || !current_task)
        return -EINVAL;

    while (true) {
        wait_queue_entry_t wait;

        if (socket_pending_take(listen_sock, EPOLLOUT))
            return EOK;
        if (!listen_sock->node)
            return -EINVAL;

        task_prepare_block(current_task);
        wait_queue_entry_init(&wait, current_task,
                              EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLNVAL, NULL,
                              NULL);
        wait_queue_add(&listen_sock->node->poll_wait, &wait);

        spin_lock(&listen_sock->lock);
        bool closed = listen_sock->closed || !listen_sock->connMax;
        bool available = listen_sock->connCurr < listen_sock->connMax;
        spin_unlock(&listen_sock->lock);

        if (closed) {
            wait_queue_remove(&listen_sock->node->poll_wait, &wait);
            task_cancel_block_prepare(current_task);
            return -ECONNREFUSED;
        }
        if (available) {
            wait_queue_remove(&listen_sock->node->poll_wait, &wait);
            task_cancel_block_prepare(current_task);
            return EOK;
        }
        if (task_signal_has_deliverable(current_task)) {
            wait_queue_remove(&listen_sock->node->poll_wait, &wait);
            task_cancel_block_prepare(current_task);
            return -EINTR;
        }

        int reason = task_block(current_task, TASK_BLOCKING, -1,
                                "socket_connect_backlog");
        wait_queue_remove(&listen_sock->node->poll_wait, &wait);
        task_cancel_block_prepare(current_task);

        if (reason != EOK && task_signal_has_deliverable(current_task))
            return -EINTR;
        if (reason < 0)
            return reason;
    }
}

static const char *unix_socket_local_name(const socket_t *sock) {
    if (!sock)
        return "";
    if (sock->bindAddr && sock->bindAddr[0])
        return sock->bindAddr;
    if (sock->filename && sock->filename[0])
        return sock->filename;
    return "";
}

static void unix_socket_write_sockaddr(const char *name,
                                       struct sockaddr_un *addr,
                                       socklen_t *addrlen) {
    memset(addr, 0, sizeof(struct sockaddr_un));
    addr->sun_family = 1;
    *addrlen = sizeof(addr->sun_family);

    if (!name || !name[0])
        return;

    size_t max_path = sizeof(addr->sun_path);
    size_t raw_len = strlen(name);

    if (name[0] == '@') {
        size_t n = MIN(raw_len - 1, max_path - 1);
        addr->sun_path[0] = '\0';
        if (n > 0)
            memcpy(addr->sun_path + 1, name + 1, n);
        *addrlen += 1 + n;
    } else {
        size_t n = MIN(raw_len, max_path - 1);
        memcpy(addr->sun_path, name, n);
        *addrlen += n + 1;
    }
}

int sockfs_create_handle_file(socket_handle_t *handle, unsigned int open_flags,
                              struct vfs_file **out_file) {
    struct vfs_mount *mnt;
    struct vfs_super_block *sb;
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_qstr name = {0};
    struct vfs_file *file;
    char label[64];

    if (!handle || !out_file)
        return -EINVAL;

    mnt = sockfs_get_internal_mount();
    if (!mnt)
        return -ENODEV;
    sb = mnt->mnt_sb;

    inode = vfs_alloc_inode(sb);
    if (!inode) {
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    inode->i_ino = sockfs_next_ino(sb);
    inode->inode = inode->i_ino;
    inode->i_mode = S_IFSOCK | 0600;
    inode->i_nlink = 1;
    inode->i_fop = &sockfs_file_ops;
    inode->i_private = handle;

    snprintf(label, sizeof(label), "socket-%llu",
             (unsigned long long)inode->i_ino);
    vfs_qstr_make(&name, label);
    dentry = vfs_d_alloc(sb, sb->s_root, &name);
    if (!dentry) {
        inode->i_private = NULL;
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    vfs_d_instantiate(dentry, inode);
    file = vfs_alloc_file(&(struct vfs_path){.mnt = mnt, .dentry = dentry},
                          open_flags);
    if (!file) {
        vfs_dput(dentry);
        inode->i_private = NULL;
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENOMEM;
    }

    file->private_data = handle;
    *out_file = file;

    vfs_dput(dentry);
    vfs_iput(inode);
    vfs_mntput(mnt);
    return 0;
}

socket_t *unix_socket_alloc() {
    socket_t *sock = malloc(sizeof(socket_t));
    if (!sock)
        return NULL;
    memset(sock, 0, sizeof(socket_t));
    sock->refs = 1;
    spin_init(&sock->lock);

    sock->recv_size = BUFFER_SIZE;
    skb_queue_init(&sock->recv_queue, sock->recv_size,
                   (skb_priv_destructor_t)unix_socket_ancillary_free);
    sock->node = NULL;

    // 设置凭据
    unix_socket_fill_cred_from_task(&sock->cred, current_task);
    sock->net_ns = current_task && current_task->nsproxy
                       ? current_task->nsproxy->net_ns
                       : NULL;
    task_simple_namespace_get(sock->net_ns);

    // 加入链表
    spin_lock(&unix_socket_list_lock);
    unix_socket_list_tail->next = sock;
    unix_socket_list_tail = sock;
    spin_unlock(&unix_socket_list_lock);

    return sock;
}

static void unix_socket_destroy(socket_t *sock) {
    if (!sock)
        return;

    unix_socket_unbind(sock);

    // 从链表移除
    spin_lock(&unix_socket_list_lock);
    socket_t *browse = &first_unix_socket;
    while (browse && browse->next != sock)
        browse = browse->next;
    if (browse) {
        browse->next = sock->next;
        if (unix_socket_list_tail == sock)
            unix_socket_list_tail = browse;
    }
    spin_unlock(&unix_socket_list_lock);

    // 释放资源
    skb_queue_purge(&sock->recv_queue);
    if (sock->filename)
        free(sock->filename);
    if (sock->backlog)
        free(sock->backlog);
    if (sock->filter)
        free(sock->filter);
    task_simple_namespace_put(sock->net_ns);
    if (sock->node) {
        vfs_iput(sock->node);
        sock->node = NULL;
    }

    free(sock);
}

void unix_socket_free(socket_t *sock) {
    if (!sock)
        return;

    if (__atomic_sub_fetch(&sock->refs, 1, __ATOMIC_ACQ_REL) == 0)
        unix_socket_destroy(sock);
}

static void unix_socket_handle_release(socket_handle_t *handle) {
    socket_t *sock;

    if (!handle)
        return;

    sock = (socket_t *)handle->sock;
    if (!sock) {
        free(handle);
        return;
    }

    spin_lock(&sock->lock);
    if (sock->connMax > 0 && sock->backlogCap > 0 && sock->connCurr > 0) {
        int pending = sock->connCurr;
        for (int i = 0; i < pending; i++) {
            int slot = (sock->connHead + i) % sock->backlogCap;
            socket_t *pending_sock = sock->backlog[slot];

            sock->backlog[slot] = NULL;
            if (!pending_sock)
                continue;

            unix_socket_close_owned(pending_sock);
        }
        sock->connCurr = 0;
        sock->connHead = 0;
    }
    spin_unlock(&sock->lock);

    unix_socket_close_owned(sock);
    free(handle);
}

static size_t unix_socket_send_stream_to_peer(
    socket_t *self, socket_t *peer, const uint8_t *data, size_t len, int flags,
    fd_t *fd_handle, unix_socket_ancillary_t **ancillary) {
    socket_t *active_peer = peer;

    if (!len)
        return 0;

    while (true) {
        if (self && self->shut_wr) {
            if (!(flags & MSG_NOSIGNAL))
                task_commit_signal(current_task, SIGPIPE, NULL);
            return -EPIPE;
        }

        if (!active_peer) {
            if (!(flags & MSG_NOSIGNAL))
                task_commit_signal(current_task, SIGPIPE, NULL);
            return -EPIPE;
        }

        spin_lock(&active_peer->lock);
        if (active_peer->closed || active_peer->shut_rd) {
            spin_unlock(&active_peer->lock);
            if (!(flags & MSG_NOSIGNAL))
                task_commit_signal(current_task, SIGPIPE, NULL);
            return -EPIPE;
        }

        size_t available = unix_socket_recv_space_locked(active_peer);
        if (available > 0) {
            size_t to_copy = MIN(len, available);
            unix_socket_ancillary_t *skb_ancillary = NULL;
            skb_buff_t *skb = NULL;

            if (ancillary && *ancillary) {
                skb_ancillary = *ancillary;
                *ancillary = NULL;
            }

            skb = unix_socket_build_skb(data, to_copy, skb_ancillary);
            if (!skb) {
                spin_unlock(&active_peer->lock);
                return -ENOMEM;
            }
            if (!skb_queue_push(&active_peer->recv_queue, skb)) {
                skb_free(skb,
                         (skb_priv_destructor_t)unix_socket_ancillary_free);
                spin_unlock(&active_peer->lock);
                continue;
            }

            spin_unlock(&active_peer->lock);
            socket_notify_sock(active_peer, EPOLLIN);
            return to_copy;
        }
        spin_unlock(&active_peer->lock);

        if ((fd_handle && (fd_get_flags(fd_handle) & O_NONBLOCK)) ||
            (flags & MSG_DONTWAIT)) {
            return -(EWOULDBLOCK);
        }

        if (!fd_handle)
            return -EINVAL;

        int reason = socket_wait_file(fd_handle, EPOLLOUT, "socket_send");
        if (reason != EOK)
            return -EINTR;
    }
}

static size_t unix_socket_send_skb_to_peer(socket_t *self, socket_t *peer,
                                           skb_buff_t *skb, int flags,
                                           fd_t *fd_handle) {
    socket_t *active_peer = peer;
    size_t packet_len = skb ? skb_unread_len(skb) : 0;

    if (!skb)
        return -EINVAL;

    while (true) {
        if (self && self->shut_wr) {
            if (!(flags & MSG_NOSIGNAL))
                task_commit_signal(current_task, SIGPIPE, NULL);
            skb_free(skb, (skb_priv_destructor_t)unix_socket_ancillary_free);
            return -EPIPE;
        }

        if (!active_peer) {
            if (!(flags & MSG_NOSIGNAL))
                task_commit_signal(current_task, SIGPIPE, NULL);
            skb_free(skb, (skb_priv_destructor_t)unix_socket_ancillary_free);
            return -EPIPE;
        }

        spin_lock(&active_peer->lock);
        if (active_peer->closed || active_peer->shut_rd) {
            spin_unlock(&active_peer->lock);
            if (!(flags & MSG_NOSIGNAL))
                task_commit_signal(current_task, SIGPIPE, NULL);
            skb_free(skb, (skb_priv_destructor_t)unix_socket_ancillary_free);
            return -EPIPE;
        }

        if (packet_len > active_peer->recv_size) {
            spin_unlock(&active_peer->lock);
            skb_free(skb, (skb_priv_destructor_t)unix_socket_ancillary_free);
            return -EMSGSIZE;
        }

        if (unix_socket_recv_space_locked(active_peer) >= packet_len) {
            if (!skb_queue_push(&active_peer->recv_queue, skb)) {
                spin_unlock(&active_peer->lock);
                continue;
            }

            spin_unlock(&active_peer->lock);
            socket_notify_sock(active_peer, EPOLLIN);
            return packet_len;
        }
        spin_unlock(&active_peer->lock);

        if ((fd_handle && (fd_get_flags(fd_handle) & O_NONBLOCK)) ||
            (flags & MSG_DONTWAIT)) {
            skb_free(skb, (skb_priv_destructor_t)unix_socket_ancillary_free);
            return -(EWOULDBLOCK);
        }

        if (!fd_handle) {
            skb_free(skb, (skb_priv_destructor_t)unix_socket_ancillary_free);
            return -EINVAL;
        }

        int reason = socket_wait_file(fd_handle, EPOLLOUT, "socket_send");
        if (reason != EOK) {
            skb_free(skb, (skb_priv_destructor_t)unix_socket_ancillary_free);
            return -EINTR;
        }
    }
}

static size_t unix_socket_send_to_peer(socket_t *self, socket_t *peer,
                                       const uint8_t *data, size_t len,
                                       int flags, fd_t *fd_handle,
                                       unix_socket_ancillary_t **ancillary) {
    if (self && unix_socket_is_message_type(self->type)) {
        unix_socket_ancillary_t *skb_ancillary = NULL;
        skb_buff_t *skb = NULL;
        int ret = unix_socket_ensure_sender_cred(ancillary, false);

        if (ret < 0)
            return ret;

        if (ancillary && *ancillary) {
            skb_ancillary = *ancillary;
            *ancillary = NULL;
        }

        skb = unix_socket_build_skb(data, len, skb_ancillary);
        if (!skb)
            return -ENOMEM;

        return unix_socket_send_skb_to_peer(self, peer, skb, flags, fd_handle);
    }

    return unix_socket_send_stream_to_peer(self, peer, data, len, flags,
                                           fd_handle, ancillary);
}

static size_t unix_socket_recv_from_self(socket_t *self, socket_t *peer,
                                         uint8_t *buf, size_t len, int flags,
                                         fd_t *fd_handle) {
    bool peek = !!(flags & MSG_PEEK);

    if (!self)
        return -EINVAL;
    if (self->shut_rd)
        return 0;
    if (!len && !unix_socket_is_message_type(self->type))
        return 0;

    while (true) {
        spin_lock(&self->lock);

        bool has_data = unix_socket_is_message_type(self->type)
                            ? (skb_queue_packets(&self->recv_queue) > 0)
                            : (unix_socket_recv_used_locked(self) > 0);
        if (has_data) {
            bool truncated = false;
            size_t copied =
                unix_socket_is_message_type(self->type)
                    ? unix_socket_packet_read_locked(self, buf, len, peek,
                                                     &truncated, NULL, NULL)
                    : unix_socket_stream_read_locked(self, buf, len, peek, NULL,
                                                     NULL);
            spin_unlock(&self->lock);

            if (!peek) {
                socket_t *notify_peer = unix_socket_get_peer_ref(self);
                socket_notify_sock(self, EPOLLOUT);
                if (notify_peer) {
                    socket_notify_sock(notify_peer, EPOLLOUT);
                    unix_socket_free(notify_peer);
                }
            }
            return copied;
        }

        socket_t *active_peer = peer;
        if (unix_socket_is_connected_type(self->type))
            active_peer = self->peer;
        bool eof =
            unix_socket_is_connected_type(self->type) &&
            (!active_peer || active_peer->closed || active_peer->shut_wr);
        spin_unlock(&self->lock);

        if (eof)
            return 0;

        if ((fd_handle && (fd_get_flags(fd_handle) & O_NONBLOCK)) ||
            (flags & MSG_DONTWAIT)) {
            return -(EWOULDBLOCK);
        }

        if (!fd_handle)
            return -EINVAL;
        int reason = socket_wait_file(fd_handle, EPOLLIN, "socket_recv");
        if (reason != EOK)
            return -EINTR;
    }
}

static size_t
unix_socket_recvmsg_from_self(socket_t *self, socket_t *peer,
                              struct msghdr *msg, int flags, fd_t *fd_handle,
                              unix_socket_ancillary_t **ancillary_out) {
    bool peek = !!(flags & MSG_PEEK);
    size_t len_total = 0;

    if (!self || !msg)
        return -EINVAL;
    if (ancillary_out)
        *ancillary_out = NULL;
    if (self->shut_rd)
        return 0;

    int total_ret =
        unix_socket_iov_total_len(msg->msg_iov, msg->msg_iovlen, &len_total);
    if (total_ret < 0)
        return (size_t)total_ret;
    if (!len_total && !unix_socket_is_message_type(self->type))
        return 0;

    while (true) {
        spin_lock(&self->lock);

        bool has_data = unix_socket_is_message_type(self->type)
                            ? (skb_queue_packets(&self->recv_queue) > 0)
                            : (unix_socket_recv_used_locked(self) > 0);
        if (has_data) {
            unix_socket_ancillary_t *ancillary_list = NULL;
            unix_socket_ancillary_t *ancillary_tail = NULL;
            bool truncated = false;
            size_t copied = 0;

            if (unix_socket_is_message_type(self->type)) {
                if (len_total > 0) {
                    copied = unix_socket_packet_readv_locked(
                        self, msg->msg_iov, msg->msg_iovlen, len_total, peek,
                        &truncated, &ancillary_list, &ancillary_tail);
                } else {
                    copied = unix_socket_packet_read_locked(
                        self, NULL, 0, peek, &truncated, &ancillary_list,
                        &ancillary_tail);
                }
            } else {
                copied = unix_socket_stream_readv_locked(
                    self, msg->msg_iov, msg->msg_iovlen, len_total, peek,
                    &ancillary_list, &ancillary_tail);
            }
            spin_unlock(&self->lock);

            if ((ssize_t)copied < 0) {
                unix_socket_ancillary_free_list(ancillary_list);
                return copied;
            }

            if (truncated)
                msg->msg_flags |= MSG_TRUNC;
            if (ancillary_out)
                *ancillary_out = ancillary_list;
            else
                unix_socket_ancillary_free_list(ancillary_list);

            if (!peek) {
                socket_t *notify_peer = unix_socket_get_peer_ref(self);
                socket_notify_sock(self, EPOLLOUT);
                if (notify_peer) {
                    socket_notify_sock(notify_peer, EPOLLOUT);
                    unix_socket_free(notify_peer);
                }
            }
            return copied;
        }

        socket_t *active_peer = peer;
        if (unix_socket_is_connected_type(self->type))
            active_peer = self->peer;
        bool eof =
            unix_socket_is_connected_type(self->type) &&
            (!active_peer || active_peer->closed || active_peer->shut_wr);
        spin_unlock(&self->lock);

        if (eof)
            return 0;

        if ((fd_handle && (fd_get_flags(fd_handle) & O_NONBLOCK)) ||
            (flags & MSG_DONTWAIT)) {
            return -(EWOULDBLOCK);
        }

        if (!fd_handle)
            return -EINVAL;
        int reason = socket_wait_file(fd_handle, EPOLLIN, "socket_recvmsg");
        if (reason != EOK)
            return -EINTR;
    }
}

static void unix_socket_drop_pending_file(fd_t *pending_file) {
    if (!pending_file)
        return;
    vfs_file_put(pending_file);
}

static size_t unix_socket_install_pending_files(fd_t **pending_files,
                                                size_t pending_count,
                                                int *fds_out, int *msg_flags,
                                                int recv_flags) {
    size_t installed = 0;
    fd_info_t *fd_info = task_fd_info_get(current_task);
    if (!fd_info)
        return 0;

    with_fd_info_lock(fd_info, {
        for (size_t i = 0; i < pending_count; i++) {
            int new_fd = -1;
            size_t limit = MIN(fd_info->max_fds,
                               current_task->rlim[RLIMIT_NOFILE].rlim_cur);
            for (size_t fd_idx = fd_info->next_fd; fd_idx < limit; fd_idx++) {
                if (fd_info->fds[fd_idx].file == NULL) {
                    new_fd = (int)fd_idx;
                    break;
                }
            }

            if (new_fd < 0 &&
                current_task->rlim[RLIMIT_NOFILE].rlim_cur > fd_info->max_fds) {
                size_t old_max = fd_info->max_fds;
                size_t new_max = MIN(current_task->rlim[RLIMIT_NOFILE].rlim_cur,
                                     MAX(old_max * 2, old_max + 1));
                if (task_fd_info_expand(fd_info, new_max) < 0)
                    break;
                limit = MIN(fd_info->max_fds,
                            current_task->rlim[RLIMIT_NOFILE].rlim_cur);
                for (size_t fd_idx = old_max; fd_idx < limit; fd_idx++) {
                    if (fd_info->fds[fd_idx].file == NULL) {
                        new_fd = (int)fd_idx;
                        break;
                    }
                }
            }

            if (new_fd < 0)
                break;

            fd_t *new_entry = vfs_file_get(pending_files[i]);
            if (!new_entry)
                break;
            if (task_fd_slot_install(
                    fd_info, new_fd, new_entry,
                    (recv_flags & MSG_CMSG_CLOEXEC) ? FD_CLOEXEC : 0) < 0) {
                vfs_file_put(new_entry);
                break;
            }
            vfs_file_put(new_entry);
            fds_out[installed++] = new_fd;
        }
    });
    task_fd_info_put(fd_info, current_task);

    for (size_t i = 0; i < installed; i++) {
        vfs_file_put(pending_files[i]);
        pending_files[i] = NULL;
        on_open_file_call(current_task, fds_out[i]);
    }

    if (installed < pending_count) {
        if (msg_flags)
            *msg_flags |= MSG_CTRUNC;
        for (size_t i = installed; i < pending_count; i++) {
            unix_socket_drop_pending_file(pending_files[i]);
            pending_files[i] = NULL;
        }
    }

    return installed;
}

int socket_socket(int domain, int type, int protocol) {
    int sock_type = type & SOCK_TYPE_MASK;
    if (!unix_socket_type_supported(sock_type)) {
        return -ESOCKTNOSUPPORT;
    }

    socket_t *sock = unix_socket_alloc();
    if (!sock)
        return -ENOMEM;

    sock->domain = domain;
    sock->type = sock_type;
    sock->protocol = protocol;

    struct vfs_file *file = NULL;
    socket_handle_t *handle = calloc(1, sizeof(*handle));
    uint64_t flags = O_RDWR;
    if (type & SOCK_NONBLOCK_FLAG)
        flags |= O_NONBLOCK;

    if (!handle) {
        unix_socket_free(sock);
        return -ENOMEM;
    }
    handle->op = &socket_ops;
    handle->sock = sock;
    handle->read_op = socket_read;
    handle->write_op = socket_write;
    handle->ioctl_op = socket_ioctl;
    handle->poll_op = socket_poll;
    handle->release = unix_socket_handle_release;

    int ret = sockfs_create_handle_file(handle, flags, &file);
    if (ret < 0) {
        free(handle);
        unix_socket_free(sock);
        return ret;
    }
    sock->node = vfs_igrab(file->f_inode);

    ret = task_install_file(current_task, file,
                            (type & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    return ret;
}

int socket_bind(uint64_t fd, const struct sockaddr_un *addr,
                socklen_t addrlen) {
    if (!addr)
        return -EFAULT;

    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file)
        return -EBADF;
    socket_handle_t *handle = sockfs_file_handle(file);
    socket_t *sock = handle->sock;

    if (sock->bindAddr) {
        vfs_file_put(file);
        return -EINVAL;
    }

    char *safe = unix_socket_addr_safe(addr, addrlen);
    if (((uint64_t)safe & ERRNO_MASK) == ERRNO_MASK) {
        vfs_file_put(file);
        return (uint64_t)safe;
    }

    bool is_abstract = (addr->sun_path[0] == '\0');
    size_t safeLen = strlen(safe);

    if (!is_abstract) {
        struct vfs_path existing = {0};
        if (vfs_filename_lookup(AT_FDCWD, safe, LOOKUP_FOLLOW, &existing) ==
            0) {
            vfs_path_put(&existing);
            free(safe);
            vfs_file_put(file);
            return -EADDRINUSE;
        }
        int mkret = vfs_mknodat(AT_FDCWD, safe, S_IFSOCK | 0666, 0, true);
        if (mkret < 0) {
            free(safe);
            vfs_file_put(file);
            return mkret;
        }
    }

    uint64_t bind_hash = unix_socket_name_hash(safe);
    spin_lock(&unix_socket_bind_lock);
    if (unix_socket_lookup_bound_locked(safe, safeLen, sock, false)) {
        spin_unlock(&unix_socket_bind_lock);
        free(safe);
        vfs_file_put(file);
        return -EADDRINUSE;
    }

    unix_socket_bind_bucket_t *bucket =
        unix_socket_bind_bucket_lookup_locked(bind_hash);
    if (!bucket) {
        bucket = calloc(1, sizeof(*bucket));
        if (!bucket) {
            spin_unlock(&unix_socket_bind_lock);
            if (!is_abstract)
                unix_socket_unlink_bound_path(safe);
            free(safe);
            vfs_file_put(file);
            return -ENOMEM;
        }
        bucket->hash = bind_hash;
        if (hashmap_put(&unix_socket_bind_map, bind_hash, bucket) != 0) {
            free(bucket);
            spin_unlock(&unix_socket_bind_lock);
            if (!is_abstract)
                unix_socket_unlink_bound_path(safe);
            free(safe);
            vfs_file_put(file);
            return -ENOMEM;
        }
    }

    sock->bindAddr = safe;
    sock->bindAddrLen = safeLen;
    sock->bindHash = bind_hash;
    sock->bind_next = bucket->head;
    bucket->head = sock;
    spin_unlock(&unix_socket_bind_lock);

    vfs_file_put(file);
    return 0;
}

int socket_listen(uint64_t fd, int backlog) {
    const int somaxconn = 4096;

    if (backlog == 0)
        backlog = 16;
    else if (backlog < 0 || backlog > somaxconn)
        backlog = somaxconn;

    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file)
        return -EBADF;
    socket_handle_t *handle = sockfs_file_handle(file);
    socket_t *sock = handle->sock;

    spin_lock(&sock->lock);
    unix_socket_fill_cred_from_task(&sock->cred, current_task);
    if (sock->backlog) {
        free(sock->backlog);
        sock->backlog = NULL;
    }
    sock->connMax = backlog;
    sock->connCurr = 0;
    sock->connHead = 0;
    sock->backlogCap = 0;
    spin_unlock(&sock->lock);
    vfs_file_put(file);
    return 0;
}

int socket_accept(uint64_t fd, struct sockaddr_un *addr, socklen_t *addrlen,
                  uint64_t flags) {
    fd_t *listener_fd = task_get_file(current_task, (int)fd);
    if (!listener_fd) {
        return -EBADF;
    }

    socket_handle_t *handle = sockfs_file_handle(listener_fd);
    socket_t *listen_sock = handle->sock;

    if (flags & ~(SOCK_CLOEXEC_FLAG | SOCK_NONBLOCK_FLAG)) {
        vfs_file_put(listener_fd);
        return -EINVAL;
    }

    if (!unix_socket_is_connected_type(listen_sock->type)) {
        vfs_file_put(listener_fd);
        return -EOPNOTSUPP;
    }

    if (addr && !addrlen) {
        vfs_file_put(listener_fd);
        return -EFAULT;
    }

    bool listener_nonblock = !!(fd_get_flags(listener_fd) & O_NONBLOCK);

    if (!listen_sock->connMax) {
        vfs_file_put(listener_fd);
        return -EINVAL;
    }

    // 等待连接并从 backlog 取一个
    socket_t *server_sock = NULL;
    while (true) {
        spin_lock(&listen_sock->lock);
        if (listen_sock->connCurr > 0) {
            int head = listen_sock->connHead;
            server_sock = listen_sock->backlog[head];
            listen_sock->backlog[head] = NULL;
            listen_sock->connHead =
                (listen_sock->connHead + 1) % listen_sock->backlogCap;
            listen_sock->connCurr--;
            if (listen_sock->connCurr == 0)
                listen_sock->connHead = 0;
            spin_unlock(&listen_sock->lock);
            socket_notify_sock(listen_sock, EPOLLOUT);
            break;
        }
        spin_unlock(&listen_sock->lock);
        if (fd_get_flags(listener_fd) & O_NONBLOCK) {
            vfs_file_put(listener_fd);
            return -(EWOULDBLOCK);
        }
        int reason = socket_wait_file(listener_fd, EPOLLIN, "socket_accept");
        if (reason != EOK) {
            vfs_file_put(listener_fd);
            return -EINTR;
        }
    }

    if (!server_sock) {
        vfs_file_put(listener_fd);
        return -ECONNABORTED;
    }

    struct vfs_file *accept_file = NULL;
    uint64_t accept_file_flags = O_RDWR;
    if ((flags & SOCK_NONBLOCK_FLAG) || listener_nonblock)
        accept_file_flags |= O_NONBLOCK;

    socket_handle_t *accept_handle = calloc(1, sizeof(*accept_handle));
    if (!accept_handle) {
        vfs_file_put(listener_fd);
        unix_socket_close_owned(server_sock);
        return -ENOMEM;
    }
    accept_handle->op = &socket_ops;
    accept_handle->sock = server_sock;
    accept_handle->read_op = socket_read;
    accept_handle->write_op = socket_write;
    accept_handle->ioctl_op = socket_ioctl;
    accept_handle->poll_op = socket_poll;
    accept_handle->release = unix_socket_handle_release;

    if (sockfs_create_handle_file(accept_handle, accept_file_flags,
                                  &accept_file) < 0) {
        free(accept_handle);
        vfs_file_put(listener_fd);
        unix_socket_close_owned(server_sock);
        return -ENOMEM;
    }
    server_sock->node = vfs_igrab(accept_file->f_inode);

    /*
     * The client can send its first bytes immediately after connect() returns,
     * before accept() has created and attached the server-side inode. Any
     * notification sent in that window only lands in pending_events. Replay
     * the current state after inode attachment so dbus/glib pollers don't miss
     * the authentication or RequestName traffic on newly accepted sockets.
     */
    uint32_t replay_events = 0;
    socket_t *server_peer = NULL;
    spin_lock(&server_sock->lock);
    bool has_input = unix_socket_is_message_type(server_sock->type)
                         ? (skb_queue_packets(&server_sock->recv_queue) > 0)
                         : (unix_socket_recv_used_locked(server_sock) > 0);
    if (has_input || server_sock->shut_rd)
        replay_events |= EPOLLIN | EPOLLRDNORM;
    if (server_sock->peer) {
        server_peer = server_sock->peer;
        unix_socket_get(server_peer);
        if (spin_trylock(&server_peer->lock)) {
            if (server_peer->closed)
                replay_events |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP | EPOLLHUP;
            else if (server_peer->shut_wr)
                replay_events |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
            spin_unlock(&server_peer->lock);
        }
    } else if (server_sock->established) {
        replay_events |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP | EPOLLHUP;
    }
    if (server_sock->closed)
        replay_events |= EPOLLERR | EPOLLHUP;
    spin_unlock(&server_sock->lock);
    if (replay_events)
        socket_notify_sock(server_sock, replay_events);

    int ret =
        task_install_file(current_task, accept_file,
                          (flags & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(listener_fd);
    vfs_file_put(accept_file);

    if (ret < 0) {
        if (server_peer)
            unix_socket_free(server_peer);
        return ret;
    }

    if (server_peer) {
        socket_notify_sock(server_peer, EPOLLOUT);
    }

    if (addr) {
        struct sockaddr_un kaddr;
        socklen_t kaddrlen = 0;
        const char *name = unix_socket_local_name(server_peer);
        unix_socket_write_sockaddr(name, &kaddr, &kaddrlen);

        socklen_t user_len = *addrlen;
        size_t copy_len = MIN((size_t)user_len, (size_t)kaddrlen);
        if (copy_len > 0)
            memcpy(addr, &kaddr, copy_len);
        *addrlen = kaddrlen;
    }

    if (server_peer)
        unix_socket_free(server_peer);
    return ret;
}

uint64_t socket_shutdown(uint64_t fd, uint64_t how) {
    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file)
        return -EBADF;
    if (how > SHUT_RDWR) {
        vfs_file_put(file);
        return -EINVAL;
    }

    socket_handle_t *handle = sockfs_file_handle(file);
    socket_t *sock = handle->sock;
    socket_t *peer = NULL;

    if (unix_socket_is_connected_type(sock->type) && !sock->peer &&
        !sock->established && sock->connMax == 0) {
        vfs_file_put(file);
        return -ENOTCONN;
    }

    if (how == SHUT_WR || how == SHUT_RDWR)
        peer = unix_socket_get_peer_ref(sock);

    if (how == SHUT_RD || how == SHUT_RDWR)
        sock->shut_rd = true;
    if (how == SHUT_WR || how == SHUT_RDWR)
        sock->shut_wr = true;

    if (how == SHUT_RD || how == SHUT_RDWR)
        socket_notify_sock(sock, EPOLLIN | EPOLLHUP | EPOLLRDHUP);
    if (peer) {
        socket_notify_sock(peer, EPOLLIN | EPOLLRDHUP);
        unix_socket_free(peer);
    }

    vfs_file_put(file);
    return 0;
}

int socket_connect(uint64_t fd, const struct sockaddr_un *addr,
                   socklen_t addrlen) {
    if (!addr)
        return -EFAULT;

    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file)
        return -EBADF;
    socket_handle_t *handle = sockfs_file_handle(file);
    socket_t *sock = handle->sock;

    if (sock->connMax != 0) {
        vfs_file_put(file);
        return -(ECONNREFUSED);
    }

    if (sock->peer) {
        vfs_file_put(file);
        return -(EISCONN);
    }

    char *safe = unix_socket_addr_safe(addr, addrlen);
    if (((uint64_t)safe & ERRNO_MASK) == ERRNO_MASK) {
        vfs_file_put(file);
        return (uint64_t)safe;
    }
    size_t safeLen = strlen(safe);
    bool is_abstract = (addr->sun_path[0] == '\0');
    socket_t *listen_sock = unix_socket_lookup_bound(safe, safeLen, sock, true);

    if (!listen_sock) {
        int ret = -ENOENT;
        if (!is_abstract) {
            struct vfs_path path_node = {0};
            if (vfs_filename_lookup(AT_FDCWD, safe, LOOKUP_FOLLOW,
                                    &path_node) == 0) {
                ret = -ECONNREFUSED;
                vfs_path_put(&path_node);
            }
        }
        free(safe);
        vfs_file_put(file);
        return ret;
    }
    free(safe);

    if (unix_socket_is_dgram_type(sock->type)) {
        unix_socket_fill_cred_from_task(&sock->cred, current_task);
        unix_socket_snapshot_peer_cred(sock, &listen_sock->cred);
        unix_socket_set_peer(sock, listen_sock);
        sock->established = true;
        socket_notify_sock(sock, EPOLLOUT);
        unix_socket_release_lookup_ref(listen_sock);
        vfs_file_put(file);
        return 0;
    }

    while (true) {
        spin_lock(&listen_sock->lock);
        if (listen_sock->closed || !listen_sock->connMax) {
            spin_unlock(&listen_sock->lock);
            unix_socket_release_lookup_ref(listen_sock);
            vfs_file_put(file);
            return -ECONNREFUSED;
        }
        bool queue_available = listen_sock->connCurr < listen_sock->connMax;
        spin_unlock(&listen_sock->lock);

        if (queue_available)
            break;

        if ((fd_get_flags(file) & O_NONBLOCK)) {
            unix_socket_release_lookup_ref(listen_sock);
            vfs_file_put(file);
            return -EAGAIN;
        }
        int reason = socket_wait_listen_backlog(listen_sock);
        if (reason != EOK) {
            unix_socket_release_lookup_ref(listen_sock);
            vfs_file_put(file);
            return reason < 0 ? reason : -EINTR;
        }
    }

    socket_t *server_sock = unix_socket_alloc();
    if (!server_sock) {
        unix_socket_release_lookup_ref(listen_sock);
        vfs_file_put(file);
        return -ENOMEM;
    }

    server_sock->domain = listen_sock->domain;
    server_sock->type = listen_sock->type;
    server_sock->protocol = listen_sock->protocol;
    task_simple_namespace_put(server_sock->net_ns);
    server_sock->net_ns = listen_sock->net_ns;
    task_simple_namespace_get(server_sock->net_ns);
    server_sock->cred = listen_sock->cred;
    server_sock->passcred = listen_sock->passcred;
    server_sock->timestamp_legacy = listen_sock->timestamp_legacy;
    unix_socket_fill_cred_from_task(&sock->cred, current_task);
    unix_socket_snapshot_peer_cred(sock, &server_sock->cred);
    unix_socket_snapshot_peer_cred(server_sock, &sock->cred);
    if (listen_sock->bindAddr) {
        server_sock->filename = strdup(listen_sock->bindAddr);
        if (!server_sock->filename) {
            unix_socket_release_lookup_ref(listen_sock);
            unix_socket_free(server_sock);
            vfs_file_put(file);
            return -ENOMEM;
        }
    }

    unix_socket_set_peer(server_sock, sock);
    unix_socket_set_peer(sock, server_sock);
    server_sock->established = true;
    sock->established = true;

    spin_lock(&listen_sock->lock);
    if (listen_sock->closed || !listen_sock->connMax ||
        listen_sock->connCurr >= listen_sock->connMax) {
        spin_unlock(&listen_sock->lock);
        sock->established = false;
        unix_socket_disconnect_peer_links(server_sock, false, false, false);
        unix_socket_release_lookup_ref(listen_sock);
        unix_socket_free(server_sock);
        vfs_file_put(file);
        return -ECONNREFUSED;
    }
    if (!unix_socket_backlog_reserve_locked(listen_sock,
                                            listen_sock->connCurr + 1)) {
        spin_unlock(&listen_sock->lock);
        sock->established = false;
        unix_socket_disconnect_peer_links(server_sock, false, false, false);
        unix_socket_release_lookup_ref(listen_sock);
        unix_socket_free(server_sock);
        vfs_file_put(file);
        return -ENOMEM;
    }
    int tail = (listen_sock->connHead + listen_sock->connCurr) %
               listen_sock->backlogCap;
    listen_sock->backlog[tail] = server_sock;
    listen_sock->connCurr++;
    spin_unlock(&listen_sock->lock);
    socket_notify_sock(listen_sock, EPOLLIN);
    socket_notify_sock(sock, EPOLLOUT);
    unix_socket_release_lookup_ref(listen_sock);

    vfs_file_put(file);
    return 0;
}

size_t unix_socket_sendto(uint64_t fd, uint8_t *in, size_t limit, int flags,
                          struct sockaddr_un *addr, uint32_t len) {
    fd_t *caller_fd = task_get_file(current_task, (int)fd);
    if (!caller_fd)
        return (size_t)-EBADF;
    socket_handle_t *handle = sockfs_file_handle(caller_fd);
    socket_t *sock = handle->sock;
    socket_t *peer = unix_socket_get_peer_ref(sock);
    bool peer_needs_unref = false;
    unix_socket_ancillary_t *ancillary = NULL;
    int ret;

    if (!peer) {
        if (unix_socket_is_connected_type(sock->type) && sock->established) {
            if (!(flags & MSG_NOSIGNAL))
                task_commit_signal(current_task, SIGPIPE, NULL);
            vfs_file_put(caller_fd);
            return (size_t)-EPIPE;
        }

        if (addr && len) {
            char *safe = unix_socket_addr_safe(addr, len);
            if (((uint64_t)safe & ERRNO_MASK) == ERRNO_MASK) {
                vfs_file_put(caller_fd);
                return (uint64_t)safe;
            }
            size_t safeLen = strlen(safe);
            int missing_peer = 0;
            socket_t *peer_sock =
                unix_socket_lookup_bound(safe, safeLen, sock, true);

            if (peer_sock) {
                free(safe);
                peer = peer_sock;
                peer_needs_unref = true;
                goto done;
            }

            missing_peer = unix_socket_missing_peer_error(
                safe, unix_socket_is_dgram_type(sock->type));
            free(safe);
            vfs_file_put(caller_fd);
            return (size_t)missing_peer;
        }
        vfs_file_put(caller_fd);
        return (size_t)unix_socket_missing_peer_error(
            NULL, unix_socket_is_dgram_type(sock->type));
    }

done:
    ret = unix_socket_maybe_add_passcred(peer, &ancillary);
    if (ret < 0) {
        unix_socket_put_peer_ref(peer, peer_needs_unref);
        vfs_file_put(caller_fd);
        return (size_t)ret;
    }

    ret = unix_socket_maybe_add_timestamp(peer, &ancillary);
    if (ret < 0) {
        if (ancillary)
            unix_socket_ancillary_free(ancillary);
        unix_socket_put_peer_ref(peer, peer_needs_unref);
        vfs_file_put(caller_fd);
        return (size_t)ret;
    }

    ret = unix_socket_send_to_peer(sock, peer, in, limit, flags, caller_fd,
                                   &ancillary);
    if (ancillary)
        unix_socket_ancillary_free(ancillary);
    unix_socket_put_peer_ref(peer, peer_needs_unref);
    vfs_file_put(caller_fd);
    return (size_t)ret;
}

size_t unix_socket_recvfrom(uint64_t fd, uint8_t *out, size_t limit, int flags,
                            struct sockaddr_un *addr, uint32_t *len) {
    fd_t *caller_fd = task_get_file(current_task, (int)fd);
    if (!caller_fd)
        return (size_t)-EBADF;
    socket_handle_t *handle = sockfs_file_handle(caller_fd);
    socket_t *sock = handle->sock;
    socket_t *peer = unix_socket_get_peer_ref(sock);

    if (unix_socket_is_connected_type(sock->type) && !sock->peer &&
        !sock->established && unix_socket_recv_used_locked(sock) == 0 &&
        skb_queue_packets(&sock->recv_queue) == 0) {
        unix_socket_free(peer);
        vfs_file_put(caller_fd);
        return -(ENOTCONN);
    }

    size_t ret =
        unix_socket_recv_from_self(sock, peer, out, limit, flags, caller_fd);
    unix_socket_free(peer);
    vfs_file_put(caller_fd);
    return ret;
}

size_t unix_socket_sendmsg(uint64_t fd, const struct msghdr *msg, int flags) {
    fd_t *caller_fd = task_get_file(current_task, (int)fd);
    if (!caller_fd)
        return (size_t)-EBADF;
    socket_handle_t *handle = sockfs_file_handle(caller_fd);
    socket_t *sock = handle->sock;
    socket_t *peer = unix_socket_get_peer_ref(sock);
    bool peer_needs_unref = false;
    size_t total_len = 0;
    int total_ret;

    if (!peer) {
        if (unix_socket_is_connected_type(sock->type) && sock->established) {
            if (!(flags & MSG_NOSIGNAL))
                task_commit_signal(current_task, SIGPIPE, NULL);
            vfs_file_put(caller_fd);
            return (size_t)-EPIPE;
        }

        if (msg->msg_name && msg->msg_namelen) {
            char *safe = unix_socket_addr_safe(msg->msg_name, msg->msg_namelen);
            if (((uint64_t)safe & ERRNO_MASK) == ERRNO_MASK) {
                vfs_file_put(caller_fd);
                return (uint64_t)safe;
            }
            size_t safeLen = strlen(safe);
            int missing_peer = 0;
            socket_t *peer_sock =
                unix_socket_lookup_bound(safe, safeLen, sock, true);

            if (peer_sock) {
                free(safe);
                peer = peer_sock;
                peer_needs_unref = true;
                goto done;
            }

            missing_peer = unix_socket_missing_peer_error(
                safe, unix_socket_is_dgram_type(sock->type));
            free(safe);
            vfs_file_put(caller_fd);
            return (size_t)missing_peer;
        }
        vfs_file_put(caller_fd);
        return (size_t)unix_socket_missing_peer_error(
            NULL, unix_socket_is_dgram_type(sock->type));
    }

done:
    total_ret =
        unix_socket_iov_total_len(msg->msg_iov, msg->msg_iovlen, &total_len);
    if (total_ret < 0) {
        unix_socket_put_peer_ref(peer, peer_needs_unref);
        vfs_file_put(caller_fd);
        return (size_t)total_ret;
    }
    unix_socket_ancillary_t *ancillary = NULL;
    int ancillary_ret = unix_socket_prepare_ancillary(msg, &ancillary);
    if (ancillary_ret < 0) {
        unix_socket_put_peer_ref(peer, peer_needs_unref);
        vfs_file_put(caller_fd);
        return (size_t)ancillary_ret;
    }

    ancillary_ret = unix_socket_maybe_add_passcred(peer, &ancillary);
    if (ancillary_ret < 0) {
        if (ancillary)
            unix_socket_ancillary_free(ancillary);
        unix_socket_put_peer_ref(peer, peer_needs_unref);
        vfs_file_put(caller_fd);
        return (size_t)ancillary_ret;
    }

    ancillary_ret = unix_socket_maybe_add_timestamp(peer, &ancillary);
    if (ancillary_ret < 0) {
        if (ancillary)
            unix_socket_ancillary_free(ancillary);
        unix_socket_put_peer_ref(peer, peer_needs_unref);
        vfs_file_put(caller_fd);
        return (size_t)ancillary_ret;
    }

    if (ancillary && total_len == 0 &&
        !unix_socket_is_message_type(sock->type)) {
        unix_socket_ancillary_free(ancillary);
        unix_socket_put_peer_ref(peer, peer_needs_unref);
        vfs_file_put(caller_fd);
        return (size_t)-EINVAL;
    }

    if (unix_socket_is_message_type(sock->type)) {
        /*
         * For DGRAM/SEQPACKET, the full iov describes one logical message, so
         * it is packed into a single skb before queueing.
         */
        ancillary_ret = unix_socket_ensure_sender_cred(&ancillary, false);
        if (ancillary_ret < 0) {
            if (ancillary)
                unix_socket_ancillary_free(ancillary);
            unix_socket_put_peer_ref(peer, peer_needs_unref);
            vfs_file_put(caller_fd);
            return (size_t)ancillary_ret;
        }

        skb_buff_t *skb = unix_socket_build_skb_from_iov(
            msg->msg_iov, msg->msg_iovlen, total_len, ancillary);
        size_t ret = 0;

        if (!skb) {
            unix_socket_put_peer_ref(peer, peer_needs_unref);
            vfs_file_put(caller_fd);
            return -ENOMEM;
        }

        ret = unix_socket_send_skb_to_peer(sock, peer, skb, flags, caller_fd);
        unix_socket_put_peer_ref(peer, peer_needs_unref);
        vfs_file_put(caller_fd);
        return ret;
    }

    size_t cnt = 0;
    bool noblock = !!(flags & MSG_DONTWAIT);
    unix_socket_ancillary_t *ancillary_to_attach = ancillary;

    /*
     * STREAM deliberately does not preserve sendmsg boundaries. Data may be
     * pushed in pieces, but the receiver still observes one continuous stream.
     */
    for (int i = 0; i < msg->msg_iovlen; i++) {
        struct iovec *curr = &((struct iovec *)msg->msg_iov)[i];
        size_t sent = 0;
        while (sent < curr->len) {
            const uint8_t *base = (const uint8_t *)curr->iov_base;
            size_t ret = unix_socket_send_to_peer(
                sock, peer, base + sent, curr->len - sent,
                noblock ? (flags | MSG_DONTWAIT) : flags, caller_fd,
                &ancillary_to_attach);
            if ((int64_t)ret < 0) {
                unix_socket_put_peer_ref(peer, peer_needs_unref);
                if (ancillary_to_attach)
                    unix_socket_ancillary_free(ancillary_to_attach);
                if (cnt > 0) {
                    vfs_file_put(caller_fd);
                    return cnt;
                }
                vfs_file_put(caller_fd);
                return ret;
            }
            if (ret == 0) {
                unix_socket_put_peer_ref(peer, peer_needs_unref);
                if (ancillary_to_attach)
                    unix_socket_ancillary_free(ancillary_to_attach);
                vfs_file_put(caller_fd);
                return cnt;
            }
            sent += ret;
            cnt += ret;
        }
    }

    unix_socket_put_peer_ref(peer, peer_needs_unref);
    if (ancillary_to_attach)
        unix_socket_ancillary_free(ancillary_to_attach);
    vfs_file_put(caller_fd);
    return cnt;
}

size_t unix_socket_recvmsg(uint64_t fd, struct msghdr *msg, int flags) {
    fd_t *caller_fd = task_get_file(current_task, (int)fd);
    if (!caller_fd)
        return (size_t)-EBADF;
    socket_handle_t *handle = sockfs_file_handle(caller_fd);
    socket_t *sock = handle->sock;
    if (unix_socket_is_connected_type(sock->type) && !sock->peer &&
        !sock->established && unix_socket_recv_used_locked(sock) == 0 &&
        skb_queue_packets(&sock->recv_queue) == 0) {
        vfs_file_put(caller_fd);
        return (size_t)-ENOTCONN;
    }

    msg->msg_flags = 0;
    unix_socket_ancillary_t *ancillary_list = NULL;
    /*
     * The recv path splits by socket type in unix_socket_recvmsg_from_self():
     * STREAM has no message boundary, while DGRAM/SEQPACKET do.
     */
    size_t cnt = unix_socket_recvmsg_from_self(sock, NULL, msg, flags,
                                               caller_fd, &ancillary_list);
    if ((int64_t)cnt < 0) {
        vfs_file_put(caller_fd);
        return cnt;
    }

    bool ancillary_deliverable = false;
    for (unix_socket_ancillary_t *anc = ancillary_list; anc != NULL;
         anc = anc->next) {
        if (anc->file_count > 0 ||
            (anc->has_cred && (sock->passcred || anc->cred_explicit)) ||
            anc->has_timestamp) {
            ancillary_deliverable = true;
            break;
        }
    }

    if (ancillary_deliverable && msg->msg_control &&
        msg->msg_controllen >= sizeof(struct cmsghdr)) {
        size_t controllen_used = 0;
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
        bool emitted_cred = false;
        bool emitted_timestamp = false;

        for (unix_socket_ancillary_t *anc = ancillary_list; anc != NULL;
             anc = anc->next) {
            if (anc->file_count > 0) {
                size_t space_left = msg->msg_controllen - controllen_used;
                if (cmsg &&
                    space_left >= CMSG_SPACE(anc->file_count * sizeof(int))) {
                    int *fds_out = (int *)CMSG_DATA(cmsg);
                    size_t installed = unix_socket_install_pending_files(
                        anc->files, anc->file_count, fds_out, &msg->msg_flags,
                        flags);
                    anc->file_count = 0;

                    if (installed > 0) {
                        cmsg->cmsg_level = SOL_SOCKET;
                        cmsg->cmsg_type = SCM_RIGHTS;
                        cmsg->cmsg_len = CMSG_LEN(installed * sizeof(int));
                        controllen_used += CMSG_SPACE(installed * sizeof(int));
                        cmsg = CMSG_NXTHDR(msg, cmsg);
                    }
                } else {
                    msg->msg_flags |= MSG_CTRUNC;
                }
            }

            if (anc->has_cred && (sock->passcred || anc->cred_explicit)) {
                if (emitted_cred) {
                    continue;
                }

                size_t space_left = msg->msg_controllen - controllen_used;
                if (cmsg && space_left >= CMSG_SPACE(sizeof(struct ucred))) {
                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SCM_CREDENTIALS;
                    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
                    memcpy(CMSG_DATA(cmsg), &anc->cred, sizeof(struct ucred));
                    controllen_used += CMSG_SPACE(sizeof(struct ucred));
                    cmsg = CMSG_NXTHDR(msg, cmsg);
                    emitted_cred = true;
                } else {
                    msg->msg_flags |= MSG_CTRUNC;
                }
            }

            if (anc->has_timestamp) {
                size_t space_left;

                if (emitted_timestamp)
                    continue;

                space_left = msg->msg_controllen - controllen_used;
                if (cmsg && space_left >= CMSG_SPACE(sizeof(struct timeval))) {
                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SO_TIMESTAMP_OLD;
                    cmsg->cmsg_len = CMSG_LEN(sizeof(struct timeval));
                    memcpy(CMSG_DATA(cmsg), &anc->timestamp,
                           sizeof(struct timeval));
                    controllen_used += CMSG_SPACE(sizeof(struct timeval));
                    cmsg = CMSG_NXTHDR(msg, cmsg);
                    emitted_timestamp = true;
                } else {
                    msg->msg_flags |= MSG_CTRUNC;
                }
            }
        }

        msg->msg_controllen = controllen_used;
    } else {
        if (ancillary_deliverable)
            msg->msg_flags |= MSG_CTRUNC;
        msg->msg_controllen = 0;
    }

    unix_socket_ancillary_free_list(ancillary_list);
    vfs_file_put(caller_fd);
    return cnt;
}

int socket_poll(fd_t *fd, size_t events) {
    socket_handle_t *handler = sockfs_file_handle(fd);
    if (!handler || !handler->sock)
        return EPOLLNVAL;
    socket_t *sock = handler->sock;
    uint32_t want = socket_expand_events((uint32_t)events);
    int revents = 0;

    if (sock->connMax > 0) {
        // listen 模式
        spin_lock(&sock->lock);
        if (sock->connCurr > 0)
            revents |= socket_ready_input(want);
        if (sock->connCurr < sock->connMax)
            revents |= socket_ready_output(want);
        if (sock->closed)
            revents |= EPOLLERR | EPOLLHUP;
        spin_unlock(&sock->lock);
    } else if (unix_socket_is_dgram_type(sock->type)) {
        spin_lock(&sock->lock);
        socket_t *peer = sock->peer;
        if (peer)
            unix_socket_get(peer);
        if (socket_wants_output(want) && !sock->closed && !sock->shut_wr)
            revents |= socket_ready_output(want);

        if (socket_wants_input(want) &&
            skb_queue_packets(&sock->recv_queue) > 0)
            revents |= socket_ready_input(want);

        if (sock->closed || sock->shut_rd)
            revents |= EPOLLERR | EPOLLHUP;
        if (!peer && sock->established)
            revents |= EPOLLHUP;
        else if (peer && spin_trylock(&peer->lock)) {
            if (peer->closed || peer->shut_wr) {
                if (socket_wants_input(want))
                    revents |= socket_ready_input(want);
                revents |= EPOLLHUP | EPOLLRDHUP;
            }
            spin_unlock(&peer->lock);
        }

        spin_unlock(&sock->lock);
        if (peer)
            unix_socket_free(peer);
    } else {
        spin_lock(&sock->lock);
        socket_t *peer = sock->peer;
        if (peer)
            unix_socket_get(peer);
        if (peer) {
            if (spin_trylock(&peer->lock)) {
                if (peer->closed)
                    revents |= EPOLLHUP | EPOLLRDHUP;
                else if (peer->shut_wr)
                    revents |= EPOLLRDHUP;
                if (socket_wants_output(want) && !sock->shut_wr &&
                    !peer->closed && unix_socket_recv_space_locked(peer) > 0)
                    revents |= socket_ready_output(want);

                bool has_input =
                    unix_socket_is_message_type(sock->type)
                        ? (skb_queue_packets(&sock->recv_queue) > 0)
                        : (unix_socket_recv_used_locked(sock) > 0);
                if (socket_wants_input(want) && (has_input || sock->shut_rd ||
                                                 peer->shut_wr || peer->closed))
                    revents |= socket_ready_input(want);
                spin_unlock(&peer->lock);
            } else {
                bool has_input =
                    unix_socket_is_message_type(sock->type)
                        ? (skb_queue_packets(&sock->recv_queue) > 0)
                        : (unix_socket_recv_used_locked(sock) > 0);
                if (socket_wants_input(want) && has_input)
                    revents |= socket_ready_input(want);
                if (sock->closed)
                    revents |= EPOLLHUP | EPOLLERR;
            }
            spin_unlock(&sock->lock);
            unix_socket_free(peer);
        } else {
            if (socket_wants_input(want) && sock->established)
                revents |= socket_ready_input(want);
            if (sock->established)
                revents |= EPOLLRDHUP;
            if (sock->established || sock->closed || sock->shut_rd ||
                sock->shut_wr)
                revents |= EPOLLHUP;
            if (sock->closed)
                revents |= EPOLLERR;
            spin_unlock(&sock->lock);
        }
    }

    return revents;
}

int socket_ioctl(fd_t *fd, ssize_t cmd, ssize_t arg) {
    vfs_node_t *node = fd->node;
    socket_handle_t *handler = sockfs_file_handle(fd);
    if (!handler || !handler->sock)
        return -EBADF;

    socket_t *sock = handler->sock;

    switch (cmd & 0xFFFFFFFF) {
    case SIOCGIFFLAGS:
        if (!arg)
            return -EFAULT;
        {
            naos_ifreq_t req;
            netdev_t *dev;

            if (copy_from_user(&req, (const void *)arg, sizeof(req)))
                return -EFAULT;

            req.ifr_name[IFNAMSIZ - 1] = '\0';
            dev = netdev_get_by_name(req.ifr_name);
            if (!dev)
                return -ENODEV;

            req.ifr_ifru.ifru_flags = (short)socket_netdev_flags(dev);
            netdev_put(dev);

            if (copy_to_user((void *)arg, &req, sizeof(req)))
                return -EFAULT;
            return 0;
        }
    case SIOCSIFFLAGS:
        if (!arg)
            return -EFAULT;
        {
            naos_ifreq_t req;
            netdev_t *dev;
            int ret;

            if (copy_from_user(&req, (const void *)arg, sizeof(req)))
                return -EFAULT;

            req.ifr_name[IFNAMSIZ - 1] = '\0';
            dev = netdev_get_by_name(req.ifr_name);
            if (!dev)
                return -ENODEV;

            ret = netdev_set_admin_state(dev,
                                         !!(req.ifr_ifru.ifru_flags & IFF_UP));
            netdev_put(dev);
            return ret;
        }
    case SIOCGIFHWADDR:
        if (!arg)
            return -EFAULT;
        {
            naos_ifreq_t req;
            netdev_t *dev;

            if (copy_from_user(&req, (const void *)arg, sizeof(req)))
                return -EFAULT;

            req.ifr_name[IFNAMSIZ - 1] = '\0';
            dev = netdev_get_by_name(req.ifr_name);
            if (!dev)
                return -ENODEV;

            memset(&req.ifr_ifru.ifru_hwaddr, 0,
                   sizeof(req.ifr_ifru.ifru_hwaddr));
            req.ifr_ifru.ifru_hwaddr.sa_family = ARPHRD_ETHER;
            memcpy(req.ifr_ifru.ifru_hwaddr.sa_data, dev->mac,
                   sizeof(dev->mac));
            netdev_put(dev);

            if (copy_to_user((void *)arg, &req, sizeof(req)))
                return -EFAULT;
            return 0;
        }
    case SIOCGIFMTU:
        if (!arg)
            return -EFAULT;
        {
            naos_ifreq_t req;
            netdev_t *dev;

            if (copy_from_user(&req, (const void *)arg, sizeof(req)))
                return -EFAULT;

            req.ifr_name[IFNAMSIZ - 1] = '\0';
            dev = netdev_get_by_name(req.ifr_name);
            if (!dev)
                return -ENODEV;

            req.ifr_ifru.ifru_mtu = (int)dev->mtu;
            netdev_put(dev);

            if (copy_to_user((void *)arg, &req, sizeof(req)))
                return -EFAULT;
            return 0;
        }
    case SIOCGIFINDEX:
        if (!arg)
            return -EFAULT;
        {
            naos_ifreq_t req;
            netdev_t *dev;

            if (copy_from_user(&req, (const void *)arg, sizeof(req)))
                return -EFAULT;

            req.ifr_name[IFNAMSIZ - 1] = '\0';
            dev = netdev_get_by_name(req.ifr_name);
            if (!dev)
                return -ENODEV;

            req.ifr_ifru.ifru_ifindex = (int)(dev->id + 1);
            netdev_put(dev);

            if (copy_to_user((void *)arg, &req, sizeof(req)))
                return -EFAULT;
            return 0;
        }
    case SIOCGIFNAME:
        if (!arg)
            return -EFAULT;
        {
            naos_ifreq_t req;
            netdev_t *dev;

            if (copy_from_user(&req, (const void *)arg, sizeof(req)))
                return -EFAULT;

            dev = netdev_get_by_index((uint32_t)(req.ifr_ifru.ifru_ifindex));
            if (!dev)
                return -ENODEV;

            memset(req.ifr_name, 0, IFNAMSIZ);
            strncpy(req.ifr_name, dev->name, IFNAMSIZ - 1);
            netdev_put(dev);

            if (copy_to_user((void *)arg, &req, sizeof(req)))
                return -EFAULT;
            return 0;
        }
    case FIONREAD:
        if (!arg)
            return -EFAULT;
        {
            spin_lock(&sock->lock);
            int value = (int)(unix_socket_is_message_type(sock->type)
                                  ? unix_socket_next_packet_len_locked(sock)
                                  : unix_socket_recv_used_locked(sock));
            spin_unlock(&sock->lock);
            if (copy_to_user((void *)arg, &value, sizeof(value)))
                return -EFAULT;
            return 0;
        }
    case TIOCOUTQ:
        if (!arg)
            return -EFAULT;
        {
            int value = 0;
            socket_t *peer = unix_socket_get_peer_ref(sock);

            if (peer) {
                spin_lock(&peer->lock);
                size_t queued = unix_socket_recv_used_locked(peer);
                value = (int)MIN(queued, (size_t)INT_MAX);
                spin_unlock(&peer->lock);
                unix_socket_free(peer);
            }

            if (copy_to_user((void *)arg, &value, sizeof(value)))
                return -EFAULT;
            return 0;
        }
    case FIONBIO:
        return 0;
    case TCGETS2:
        if (!arg)
            return -EFAULT;
        {
            struct termios2 tio = {0};
            if (copy_to_user((void *)arg, &tio, sizeof(tio)))
                return -EFAULT;
            return 0;
        }
    case SIOCGSKNS:
        if (!sock->net_ns)
            return -EINVAL;
        return procfs_create_nsfd_for_netns(sock->net_ns);
    case TIOCGPGRP:
    case TIOCGWINSZ:
        return -ENOTTY;
    default:
        printk("Unsupported unix socket ioctl cmd = %#010x\n", cmd);
        return -ENOTTY;
    }
}

ssize_t socket_read(fd_t *fd, void *buf, size_t offset, size_t limit) {
    socket_handle_t *handle = sockfs_file_handle(fd);
    socket_t *sock = handle->sock;
    socket_t *peer = unix_socket_get_peer_ref(sock);

    if (unix_socket_is_connected_type(sock->type) && !sock->peer &&
        !sock->established && unix_socket_recv_used_locked(sock) == 0 &&
        skb_queue_packets(&sock->recv_queue) == 0) {
        unix_socket_free(peer);
        return -(ENOTCONN);
    }

    ssize_t ret = unix_socket_recv_from_self(sock, peer, buf, limit, 0, fd);
    unix_socket_free(peer);
    return ret;
}

ssize_t socket_write(fd_t *fd, const void *buf, size_t offset, size_t limit) {
    socket_handle_t *handle = sockfs_file_handle(fd);
    socket_t *sock = handle->sock;
    socket_t *peer = unix_socket_get_peer_ref(sock);
    unix_socket_ancillary_t *ancillary = NULL;
    int ret;

    if (!peer) {
        if (unix_socket_is_dgram_type(sock->type))
            return -(EDESTADDRREQ);
        if (!unix_socket_is_dgram_type(sock->type) && sock->established) {
            task_commit_signal(current_task, SIGPIPE, NULL);
            return -(EPIPE);
        }
        return -(ENOTCONN);
    }

    ret = unix_socket_maybe_add_passcred(peer, &ancillary);
    if (ret < 0) {
        unix_socket_free(peer);
        return ret;
    }

    ret = unix_socket_maybe_add_timestamp(peer, &ancillary);
    if (ret < 0) {
        if (ancillary)
            unix_socket_ancillary_free(ancillary);
        unix_socket_free(peer);
        return ret;
    }

    ret = unix_socket_send_to_peer(sock, peer, buf, limit, 0, fd, &ancillary);
    if (ancillary)
        unix_socket_ancillary_free(ancillary);
    unix_socket_free(peer);
    return ret;
}

int unix_socket_pair(int domain, int type, int protocol, int *sv) {
    int sock_type = type & SOCK_TYPE_MASK;
    if (!unix_socket_type_supported(sock_type)) {
        return -ESOCKTNOSUPPORT;
    }

    socket_t *sock1 = unix_socket_alloc();
    socket_t *sock2 = unix_socket_alloc();
    if (!sock1 || !sock2) {
        unix_socket_free(sock1);
        unix_socket_free(sock2);
        return -ENOMEM;
    }

    sock1->domain = domain;
    sock1->type = sock_type;
    sock1->protocol = protocol;

    sock2->domain = domain;
    sock2->type = sock_type;
    sock2->protocol = protocol;

    // 双向连接
    unix_socket_set_peer(sock1, sock2);
    unix_socket_set_peer(sock2, sock1);
    sock1->established = true;
    sock2->established = true;
    unix_socket_snapshot_peer_cred(sock1, &sock2->cred);
    unix_socket_snapshot_peer_cred(sock2, &sock1->cred);

    uint64_t flags = O_RDWR;
    if (type & SOCK_NONBLOCK_FLAG)
        flags |= O_NONBLOCK;

    struct vfs_file *file1 = NULL;
    struct vfs_file *file2 = NULL;
    socket_handle_t *handle1 = calloc(1, sizeof(*handle1));
    socket_handle_t *handle2 = calloc(1, sizeof(*handle2));
    if (!handle1 || !handle2) {
        free(handle1);
        free(handle2);
        unix_socket_disconnect_peer_links(sock1, false, false, false);
        sock1->established = false;
        sock2->established = false;
        unix_socket_free(sock1);
        unix_socket_free(sock2);
        return -ENOMEM;
    }
    handle1->op = &socket_ops;
    handle1->sock = sock1;
    handle1->read_op = socket_read;
    handle1->write_op = socket_write;
    handle1->ioctl_op = socket_ioctl;
    handle1->poll_op = socket_poll;
    handle1->release = unix_socket_handle_release;
    handle2->op = &socket_ops;
    handle2->sock = sock2;
    handle2->read_op = socket_read;
    handle2->write_op = socket_write;
    handle2->ioctl_op = socket_ioctl;
    handle2->poll_op = socket_poll;
    handle2->release = unix_socket_handle_release;

    if (sockfs_create_handle_file(handle1, flags, &file1) < 0) {
        free(handle1);
        free(handle2);
        unix_socket_disconnect_peer_links(sock1, false, false, false);
        sock1->established = false;
        sock2->established = false;
        unix_socket_free(sock1);
        unix_socket_free(sock2);
        return -ENOMEM;
    }

    if (sockfs_create_handle_file(handle2, flags, &file2) < 0) {
        free(handle2);
        unix_socket_disconnect_peer_links(sock1, false, false, false);
        sock1->established = false;
        sock2->established = false;
        vfs_file_put(file1);
        unix_socket_free(sock2);
        return -ENOMEM;
    }
    sock1->node = vfs_igrab(file1->f_inode);
    sock2->node = vfs_igrab(file2->f_inode);

    int fd1 = -1, fd2 = -1;
    int ret = task_install_file(current_task, file1,
                                (type & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0, 0);
    if (ret >= 0)
        fd1 = ret;
    if (ret >= 0)
        ret = task_install_file(current_task, file2,
                                (type & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0,
                                fd1 + 1);
    if (ret >= 0)
        fd2 = ret;

    if (ret < 0 || fd1 < 0 || fd2 < 0) {
        vfs_file_put(file1);
        vfs_file_put(file2);
        if (fd1 >= 0)
            task_close_file_descriptor(current_task, fd1);
        return ret;
    }

    vfs_file_put(file1);
    vfs_file_put(file2);

    sv[0] = fd1;
    sv[1] = fd2;

    return 0;
}

int unix_socket_getsockname(uint64_t fd, struct sockaddr_un *addr,
                            socklen_t *addrlen) {
    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file)
        return -(EBADF);

    socket_handle_t *handle = sockfs_file_handle(file);
    socket_t *sock = handle->sock;

    unix_socket_write_sockaddr(unix_socket_local_name(sock), addr, addrlen);

    vfs_file_put(file);
    return 0;
}

size_t unix_socket_getpeername(uint64_t fd, struct sockaddr_un *addr,
                               socklen_t *len) {
    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file)
        return (size_t)-EBADF;

    socket_handle_t *handle = sockfs_file_handle(file);
    socket_t *sock = handle->sock;
    socket_t *peer = unix_socket_get_peer_ref(sock);

    if (!peer) {
        vfs_file_put(file);
        return -ENOTCONN;
    }

    unix_socket_write_sockaddr(unix_socket_local_name(peer), addr, len);
    unix_socket_free(peer);

    vfs_file_put(file);
    return 0;
}

size_t unix_socket_setsockopt(uint64_t fd, int level, int optname,
                              const void *optval, socklen_t optlen) {
    if (level != SOL_SOCKET) {
        // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
        //        __LINE__, level, optname);
        return -ENOPROTOOPT;
    }

    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file)
        return (size_t)-EBADF;

    socket_handle_t *handle = sockfs_file_handle(file);
    socket_t *sock = handle->sock;
#define UNIX_SOCKET_SETSOCKOPT_RETURN(value)                                   \
    do {                                                                       \
        vfs_file_put(file);                                                    \
        return (value);                                                        \
    } while (0)

    switch (optname) {
    case SO_REUSEADDR:
        if (optlen < sizeof(int))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        sock->reuseaddr = *(int *)optval;
        break;

    case SO_KEEPALIVE:
        if (optlen < sizeof(int))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        sock->keepalive = *(int *)optval;
        break;

    case SO_SNDTIMEO_OLD:
    case SO_SNDTIMEO_NEW:
        if (optlen < sizeof(struct timeval))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        memcpy(&sock->sndtimeo, optval, sizeof(struct timeval));
        break;

    case SO_RCVTIMEO_OLD:
    case SO_RCVTIMEO_NEW:
        if (optlen < sizeof(struct timeval))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        memcpy(&sock->rcvtimeo, optval, sizeof(struct timeval));
        break;

    case SO_BINDTODEVICE:
        if (optlen > IFNAMSIZ)
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        strncpy(sock->bind_to_dev, optval, optlen);
        sock->bind_to_dev[IFNAMSIZ - 1] = '\0';
        break;

    case SO_LINGER:
        if (optlen < sizeof(struct linger))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        memcpy(&sock->linger_opt, optval, sizeof(struct linger));
        break;

    case SO_SNDBUF:
    case SO_SNDBUFFORCE:
    case SO_RCVBUF:
    case SO_RCVBUFFORCE:
        if (optlen < sizeof(int))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        {
            int new_size = *(int *)optval;
            if (new_size < BUFFER_SIZE)
                new_size = BUFFER_SIZE;

            spin_lock(&sock->lock);
            sock->recv_size = new_size;
            skb_queue_set_limit(&sock->recv_queue, sock->recv_size);
            spin_unlock(&sock->lock);
        }
        break;

    case SO_PASSCRED:
        if (optlen < sizeof(int))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        sock->passcred = *(int *)optval;
        break;

    case SO_TIMESTAMP_OLD:
        if (optlen < sizeof(int))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        sock->timestamp_legacy = *(int *)optval;
        break;

    case SO_PRIORITY:
        break;

    case SO_PEEK_OFF:
        if (optlen < sizeof(int))
            UNIX_SOCKET_SETSOCKOPT_RETURN(-EINVAL);
        break;

    case SO_PEERCRED:
        UNIX_SOCKET_SETSOCKOPT_RETURN(-ENOPROTOOPT); // 只读

    default:
        // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
        //        __LINE__, level, optname);
        UNIX_SOCKET_SETSOCKOPT_RETURN(-ENOPROTOOPT);
    }

    vfs_file_put(file);
#undef UNIX_SOCKET_SETSOCKOPT_RETURN
    return 0;
}

size_t unix_socket_getsockopt(uint64_t fd, int level, int optname, void *optval,
                              socklen_t *optlen) {
    if (level != SOL_SOCKET) {
        // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
        //        __LINE__, level, optname);
        return -ENOPROTOOPT;
    }

    fd_t *file = task_get_file(current_task, (int)fd);
    if (!file)
        return (size_t)-EBADF;

    socket_handle_t *handle = sockfs_file_handle(file);
    socket_t *sock = handle->sock;
#define UNIX_SOCKET_GETSOCKOPT_RETURN(value)                                   \
    do {                                                                       \
        vfs_file_put(file);                                                    \
        return (value);                                                        \
    } while (0)

    switch (optname) {
    case SO_ERROR:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = 0;
        *optlen = sizeof(int);
        break;

    case SO_REUSEADDR:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = sock->reuseaddr;
        *optlen = sizeof(int);
        break;

    case SO_KEEPALIVE:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = sock->keepalive;
        *optlen = sizeof(int);
        break;

    case SO_SNDTIMEO_OLD:
    case SO_SNDTIMEO_NEW:
        if (*optlen < sizeof(struct timeval))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        memcpy(optval, &sock->sndtimeo, sizeof(struct timeval));
        *optlen = sizeof(struct timeval);
        break;

    case SO_RCVTIMEO_OLD:
    case SO_RCVTIMEO_NEW:
        if (*optlen < sizeof(struct timeval))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        memcpy(optval, &sock->rcvtimeo, sizeof(struct timeval));
        *optlen = sizeof(struct timeval);
        break;

    case SO_BINDTODEVICE:
        if (*optlen < IFNAMSIZ)
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        strncpy(optval, sock->bind_to_dev, IFNAMSIZ);
        *optlen = strlen(sock->bind_to_dev) + 1;
        break;

    case SO_PROTOCOL:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = sock->protocol;
        *optlen = sizeof(int);
        break;

    case SO_DOMAIN:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = sock->domain;
        *optlen = sizeof(int);
        break;

    case SO_LINGER:
        if (*optlen < sizeof(struct linger))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        memcpy(optval, &sock->linger_opt, sizeof(struct linger));
        *optlen = sizeof(struct linger);
        break;

    case SO_SNDBUF:
    case SO_SNDBUFFORCE:
    case SO_RCVBUF:
    case SO_RCVBUFFORCE:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = sock->recv_size;
        *optlen = sizeof(int);
        break;

    case SO_PASSCRED:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = sock->passcred;
        *optlen = sizeof(int);
        break;

    case SO_TIMESTAMP_OLD:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = sock->timestamp_legacy;
        *optlen = sizeof(int);
        break;

    case SO_PEERCRED: {
        struct ucred peer_cred = {0};
        if (!unix_socket_get_peer_cred(sock, &peer_cred))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-ENOTCONN);
        if (*optlen < sizeof(struct ucred))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        memcpy(optval, &peer_cred, sizeof(struct ucred));
        *optlen = sizeof(struct ucred);
    } break;

    case SO_PEERPIDFD: {
        struct ucred peer_cred = {0};
        if (!unix_socket_get_peer_cred(sock, &peer_cred))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-ENOTCONN);
        if (peer_cred.pid <= 0)
            UNIX_SOCKET_GETSOCKOPT_RETURN(-ESRCH);
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);

        uint64_t pidfd = pidfd_create_for_pid((uint64_t)peer_cred.pid, 0, true);
        if ((int64_t)pidfd < 0)
            UNIX_SOCKET_GETSOCKOPT_RETURN((int64_t)pidfd);

        *(int *)optval = (int)pidfd;
        *optlen = sizeof(int);
    } break;

    case SO_ACCEPTCONN:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = (sock->connMax > 0) ? 1 : 0;
        *optlen = sizeof(int);
        break;

    case SO_TYPE:
        if (*optlen < sizeof(int))
            UNIX_SOCKET_GETSOCKOPT_RETURN(-EINVAL);
        *(int *)optval = sock->type;
        *optlen = sizeof(int);
        break;

    default:
        // printk("%s:%d Unsupported optlevel or optname %d %d\n", __FILE__,
        //        __LINE__, level, optname);
        UNIX_SOCKET_GETSOCKOPT_RETURN(-ENOPROTOOPT);
    }

    vfs_file_put(file);
#undef UNIX_SOCKET_GETSOCKOPT_RETURN
    return 0;
}

socket_op_t socket_ops = {
    .shutdown = socket_shutdown,
    .accept = socket_accept,
    .listen = socket_listen,
    .getsockname = unix_socket_getsockname,
    .bind = socket_bind,
    .connect = socket_connect,
    .sendto = unix_socket_sendto,
    .recvfrom = unix_socket_recvfrom,
    .sendmsg = unix_socket_sendmsg,
    .recvmsg = unix_socket_recvmsg,
    .getpeername = unix_socket_getpeername,
    .getsockopt = unix_socket_getsockopt,
    .setsockopt = unix_socket_setsockopt,
};

static ssize_t sockfs_read(struct vfs_file *file, void *buf, size_t count,
                           loff_t *ppos) {
    socket_handle_t *handle = sockfs_file_handle(file);

    (void)ppos;
    if (!handle || !handle->read_op)
        return -EINVAL;
    return handle->read_op(file, buf, 0, count);
}

static ssize_t sockfs_write(struct vfs_file *file, const void *buf,
                            size_t count, loff_t *ppos) {
    socket_handle_t *handle = sockfs_file_handle(file);

    (void)ppos;
    if (!handle || !handle->write_op)
        return -EINVAL;
    return handle->write_op(file, buf, 0, count);
}

static long sockfs_ioctl(struct vfs_file *file, unsigned long cmd,
                         unsigned long arg) {
    socket_handle_t *handle = sockfs_file_handle(file);

    if (!handle || !handle->ioctl_op)
        return -EINVAL;
    return handle->ioctl_op(file, (ssize_t)cmd, (ssize_t)arg);
}

static __poll_t sockfs_poll(struct vfs_file *file, struct vfs_poll_table *pt) {
    socket_handle_t *handle = sockfs_file_handle(file);
    uint32_t events = EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLERR | EPOLLHUP |
                      EPOLLNVAL | EPOLLRDHUP;

    if (!handle || !handle->poll_op)
        return EPOLLNVAL;
    if (pt && pt->events)
        events = pt->events;
    return handle->poll_op(file, events);
}

static const struct vfs_file_operations sockfs_dir_file_ops = {
    .llseek = sockfs_llseek,
    .open = sockfs_open,
    .release = sockfs_release,
};

static const struct vfs_file_operations sockfs_file_ops = {
    .llseek = sockfs_llseek,
    .read = sockfs_read,
    .write = sockfs_write,
    .unlocked_ioctl = sockfs_ioctl,
    .poll = sockfs_poll,
    .open = sockfs_open,
    .release = sockfs_release,
};

void socketfs_init() {
    spin_init(&sockfs_mount_lock);
    vfs_register_filesystem(&sockfs_fs_type);
    spin_init(&unix_socket_list_lock);
    spin_init(&unix_socket_bind_lock);
    memset(&first_unix_socket, 0, sizeof(socket_t));
    unix_socket_list_tail = &first_unix_socket;
    unix_socket_bind_map = HASHMAP_INIT;
    regist_socket(1, NULL, socket_socket, unix_socket_pair);
    netlink_init();
}
