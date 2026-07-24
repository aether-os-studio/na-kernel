#include <drivers/pty.h>
#include <task/task.h>
#include <fs/vfs/vfs.h>
#include <init/callbacks.h>

uint8_t *pty_bitmap = 0;
spinlock_t pty_global_lock = SPIN_INIT;
pty_pair_t first_pair;

ssize_t pts_write_inner(fd_t *fd, uint8_t *in, size_t limit);
extern void send_process_group_signal(int pgid, int sig);

static const struct vfs_file_operations ptmx_file_ops;
static const struct vfs_file_operations pts_file_ops;
static const struct vfs_file_operations devpts_dir_file_ops;
static const struct vfs_inode_operations devpts_inode_ops;
static const struct vfs_dentry_operations devpts_dentry_ops;
static const struct vfs_super_operations devpts_super_ops;
static struct vfs_file_system_type devpts_fs_type;
int pts_ioctl(pty_pair_t *pair, uint64_t request, void *arg);

typedef struct devpts_fs_info {
    struct vfs_super_block *sb;
    struct llist_header node;
} devpts_fs_info_t;

typedef struct devpts_inode_info {
    struct vfs_inode vfs_inode;
    pty_pair_t *pair;
    struct llist_header pair_node;
} devpts_inode_info_t;

static DEFINE_LLIST(devpts_superblocks);

static inline devpts_inode_info_t *devpts_i(vfs_node_t *inode) {
    return inode ? container_of(inode, devpts_inode_info_t, vfs_inode) : NULL;
}

static inline pty_pair_t *pty_pair_from_file(fd_t *file) {
    if (!file)
        return NULL;
    if (file->private_data)
        return (pty_pair_t *)file->private_data;
    if (!file->f_inode)
        return NULL;
    return (pty_pair_t *)file->f_inode->i_private;
}

static vfs_node_t *pty_lookup_inode_path(const char *path) {
    struct vfs_path p = {0};
    vfs_node_t *inode = NULL;

    if (!path)
        return NULL;
    if (vfs_filename_lookup(AT_FDCWD, path, LOOKUP_FOLLOW, &p) < 0)
        return NULL;
    if (p.dentry && p.dentry->d_inode)
        inode = vfs_igrab(p.dentry->d_inode);
    vfs_path_put(&p);
    return inode;
}

static inline void pty_packet_queue_locked(pty_pair_t *pair, uint8_t status,
                                           uint32_t *notify_master) {
    if (!pair || !pair->packet_mode || !status)
        return;
    pair->packet_status |= status;
    if (notify_master)
        *notify_master |= EPOLLIN | EPOLLPRI;
}

static inline void pty_packet_mark_data_locked(pty_pair_t *pair,
                                               size_t old_ptr_master) {
    (void)old_ptr_master;
    if (!pair || !pair->packet_mode)
        return;
    if (pair->ptrMaster > 0)
        pair->packet_data_pending = true;
}

static inline void pty_packet_termios_changed_locked(
    pty_pair_t *pair, const struct termios *old_term, uint32_t *notify_master) {
    uint8_t status = 0;

    if (!pair || !old_term)
        return;
    if (!memcmp(old_term, &pair->term, sizeof(*old_term)))
        return;

    if ((old_term->c_iflag ^ pair->term.c_iflag) & IXON) {
        status |= (pair->term.c_iflag & IXON) ? TIOCPKT_DOSTOP : TIOCPKT_NOSTOP;
    }
    pty_packet_queue_locked(pair, status, notify_master);
}

static void pty_get_termios2_locked(const pty_pair_t *pair,
                                    struct termios2 *term2) {
    memset(term2, 0, sizeof(*term2));
    term2->c_iflag = pair->term.c_iflag;
    term2->c_oflag = pair->term.c_oflag;
    term2->c_cflag = pair->term.c_cflag;
    term2->c_lflag = pair->term.c_lflag;
    term2->c_line = pair->term.c_line;
    memcpy(term2->c_cc, pair->term.c_cc, sizeof(term2->c_cc));
    term2->c_ispeed = pair->input_speed;
    term2->c_ospeed = pair->output_speed;
}

static void pty_set_termios2_locked(pty_pair_t *pair,
                                    const struct termios2 *term2) {
    pair->term.c_iflag = term2->c_iflag;
    pair->term.c_oflag = term2->c_oflag;
    pair->term.c_cflag = term2->c_cflag;
    pair->term.c_lflag = term2->c_lflag;
    pair->term.c_line = term2->c_line;
    memcpy(pair->term.c_cc, term2->c_cc, sizeof(pair->term.c_cc));
    pair->input_speed = term2->c_ispeed;
    pair->output_speed = term2->c_ospeed;
}

static void pty_notify_master(pty_pair_t *pair, uint32_t events) {
    vfs_node_t *node = NULL;

    if (!pair || !events)
        return;

    spin_lock(&pty_global_lock);
    if (pair->ptmx_node)
        node = vfs_igrab(pair->ptmx_node);
    spin_unlock(&pty_global_lock);

    if (!node)
        return;
    vfs_poll_notify_inode(node, events);
    vfs_iput(node);
}

static void pty_notify_slaves(pty_pair_t *pair, uint32_t events) {
    devpts_inode_info_t *info, *tmp;
    vfs_node_t **nodes;
    size_t count = 0, used = 0;

    if (!pair || !events)
        return;

    spin_lock(&pty_global_lock);
    llist_for_each(info, tmp, &pair->pts_nodes, pair_node) {
        if (info->vfs_inode.i_private == pair)
            count++;
    }
    spin_unlock(&pty_global_lock);
    if (!count)
        return;

    nodes = calloc(count, sizeof(*nodes));
    if (!nodes)
        return;

    spin_lock(&pty_global_lock);
    llist_for_each(info, tmp, &pair->pts_nodes, pair_node) {
        if (used >= count)
            break;
        if (info->vfs_inode.i_private != pair)
            continue;
        nodes[used] = vfs_igrab(&info->vfs_inode);
        if (nodes[used])
            used++;
    }
    spin_unlock(&pty_global_lock);

    for (size_t i = 0; i < used; i++) {
        vfs_poll_notify_inode(nodes[i], events);
        vfs_iput(nodes[i]);
    }
    free(nodes);
}

static void pty_notify_pair(pty_pair_t *pair, uint32_t notify_master,
                            uint32_t notify_slave) {
    if (notify_master)
        pty_notify_master(pair, notify_master);
    if (notify_slave)
        pty_notify_slaves(pair, notify_slave);
}

static int pty_tcflush_locked(pty_pair_t *pair, int selector,
                              uint32_t *notify_master, uint32_t *notify_slave,
                              uint8_t *packet_status) {
    if (!pair)
        return -EINVAL;

    switch (selector) {
    case TCIFLUSH:
        pair->ptrSlave = 0;
        if (notify_master)
            *notify_master |= EPOLLOUT | EPOLLWRNORM;
        if (packet_status)
            *packet_status |= TIOCPKT_FLUSHREAD;
        return 0;
    case TCOFLUSH:
        pair->ptrMaster = 0;
        pair->packet_data_pending = false;
        if (notify_slave)
            *notify_slave |= EPOLLOUT | EPOLLWRNORM;
        if (packet_status)
            *packet_status |= TIOCPKT_FLUSHWRITE;
        return 0;
    case TCIOFLUSH:
        pair->ptrSlave = 0;
        pair->ptrMaster = 0;
        pair->packet_data_pending = false;
        if (notify_master)
            *notify_master |= EPOLLOUT | EPOLLWRNORM;
        if (notify_slave)
            *notify_slave |= EPOLLOUT | EPOLLWRNORM;
        if (packet_status)
            *packet_status |= TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE;
        return 0;
    default:
        return -EINVAL;
    }
}

static int pty_tcxonc_locked(pty_pair_t *pair, bool from_master, uintptr_t arg,
                             uint32_t *notify_master, uint32_t *notify_slave,
                             uint8_t *packet_status) {
    if (!pair)
        return -EINVAL;

    int action = (int)arg;
    switch (action) {
    case TCOOFF:
        if (from_master)
            pair->stop_master_output = true;
        else {
            pair->stop_slave_output = true;
            if (packet_status)
                *packet_status |= TIOCPKT_STOP;
        }
        return 0;
    case TCOON:
        if (from_master) {
            pair->stop_master_output = false;
            if (notify_master)
                *notify_master |= EPOLLOUT | EPOLLWRNORM;
        } else {
            pair->stop_slave_output = false;
            if (packet_status)
                *packet_status |= TIOCPKT_START;
            if (notify_slave)
                *notify_slave |= EPOLLOUT | EPOLLWRNORM;
        }
        return 0;
    case TCIOFF:
    case TCION: {
        uint8_t flow_char = pair->term.c_cc[action == TCIOFF ? VSTOP : VSTART];
        if (from_master) {
            if (!pair->slaveFds || pair->ptrSlave >= PTY_BUFF_SIZE)
                return pair->slaveFds ? -EAGAIN : 0;
            pair->bufferSlave[pair->ptrSlave++] = flow_char;
            if (notify_slave)
                *notify_slave |= EPOLLIN | EPOLLRDNORM;
        } else {
            size_t old_ptr_master = pair->ptrMaster;
            if (!pair->masterFds || pair->ptrMaster >= PTY_BUFF_SIZE)
                return pair->masterFds ? -EAGAIN : 0;
            pair->bufferMaster[pair->ptrMaster++] = flow_char;
            pty_packet_mark_data_locked(pair, old_ptr_master);
            if (notify_master)
                *notify_master |= EPOLLIN | EPOLLRDNORM;
        }
        return 0;
    }
    default:
        return -EINVAL;
    }
}

int pty_bitmap_decide() {
    int ret = -1;
    spin_lock(&pty_global_lock);
    for (int i = 0; i < PTY_MAX; i++) {
        if (!(pty_bitmap[i / 8] & (1 << (i % 8)))) {
            pty_bitmap[i / 8] |= (1 << (i % 8));
            ret = i;
            break;
        }
    }
    spin_unlock(&pty_global_lock);
    return ret;
}

void pty_bitmap_remove(int index) {
    spin_lock(&pty_global_lock);
    pty_bitmap[index / 8] &= ~(1 << (index % 8));
    spin_unlock(&pty_global_lock);
}

void pty_termios_default(struct termios *term) {
    term->c_iflag = ICRNL | IXON | BRKINT | ISTRIP | INPCK;
    term->c_oflag = OPOST | ONLCR;
    term->c_cflag = B38400 | CS8 | CREAD | HUPCL;
    term->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
    term->c_cc[VINTR] = 3;
    term->c_cc[VQUIT] = 28;
    term->c_cc[VERASE] = 127;
    term->c_cc[VKILL] = 21;
    term->c_cc[VEOF] = 4;
    term->c_cc[VTIME] = 0;
    term->c_cc[VMIN] = 1;
    term->c_cc[VSTART] = 17;
    term->c_cc[VSTOP] = 19;
    term->c_cc[VSUSP] = 26;
}

static struct vfs_inode *devpts_new_inode(struct vfs_super_block *sb,
                                          umode_t mode, pty_pair_t *pair) {
    struct vfs_inode *inode = vfs_alloc_inode(sb);
    devpts_inode_info_t *info = devpts_i(inode);

    if (!inode || !info)
        return NULL;

    inode->i_mode = mode;
    inode->i_uid = 0;
    inode->i_gid = 0;
    inode->i_nlink = S_ISDIR(mode) ? 2 : 1;
    inode->i_blkbits = 12;
    inode->i_ino = pair ? (ino64_t)pair->id + 3 : 1;
    inode->inode = inode->i_ino;
    inode->i_fop = S_ISDIR(mode) ? &devpts_dir_file_ops : &pts_file_ops;
    inode->i_op = &devpts_inode_ops;
    inode->i_rdev = pair ? ((136U << 8) | (uint32_t)pair->id) : 0;
    inode->i_private = pair;
    inode->i_atime.sec = inode->i_btime.sec = inode->i_ctime.sec =
        inode->i_mtime.sec = (int64_t)(nano_time() / 1000000000ULL);

    info->pair = pair;
    llist_init_head(&info->pair_node);
    if (pair) {
        spin_lock(&pty_global_lock);
        llist_append(&pair->pts_nodes, &info->pair_node);
        if (!pair->pts_node)
            pair->pts_node = vfs_igrab(inode);
        spin_unlock(&pty_global_lock);
    }

    return inode;
}

static struct vfs_inode *
devpts_new_slave_inode_for_id(struct vfs_super_block *sb, int id) {
    struct vfs_inode *inode = vfs_alloc_inode(sb);
    devpts_inode_info_t *info = devpts_i(inode);
    pty_pair_t *pair;

    if (!inode || !info)
        return NULL;

    inode->i_mode = S_IFCHR | 0620;
    inode->i_uid = 0;
    inode->i_gid = 0;
    inode->i_nlink = 1;
    inode->i_blkbits = 12;
    inode->i_ino = (ino64_t)id + 3;
    inode->inode = inode->i_ino;
    inode->i_fop = &pts_file_ops;
    inode->i_op = &devpts_inode_ops;
    inode->i_rdev = (136U << 8) | (uint32_t)id;
    inode->i_atime.sec = inode->i_btime.sec = inode->i_ctime.sec =
        inode->i_mtime.sec = (int64_t)(nano_time() / 1000000000ULL);

    llist_init_head(&info->pair_node);

    spin_lock(&pty_global_lock);
    for (pair = first_pair.next; pair; pair = pair->next) {
        if (pair->id == id)
            break;
    }
    if (!pair) {
        spin_unlock(&pty_global_lock);
        vfs_iput(inode);
        return NULL;
    }

    inode->i_private = pair;
    info->pair = pair;
    llist_append(&pair->pts_nodes, &info->pair_node);
    if (!pair->pts_node)
        pair->pts_node = vfs_igrab(inode);
    spin_unlock(&pty_global_lock);

    return inode;
}

static int pty_pair_install_slave_node(pty_pair_t *pair) {
    (void)pair;
    return 0;
}

static void pty_pair_cleanup(pty_pair_t *pair) {
    devpts_inode_info_t *info;
    vfs_node_t *pts_node;
    vfs_node_t *ptmx_node;

    if (!pair)
        return;

    spin_lock(&pty_global_lock);
    pts_node = pair->pts_node;
    ptmx_node = pair->ptmx_node;
    pair->pts_node = NULL;
    pair->ptmx_node = NULL;
    while (!llist_empty(&pair->pts_nodes)) {
        info = list_entry(pair->pts_nodes.next, devpts_inode_info_t, pair_node);
        llist_delete(&info->pair_node);
        info->pair = NULL;
        info->vfs_inode.i_private = NULL;
    }
    pty_pair_t *n = &first_pair;
    while (n->next && n->next != pair)
        n = n->next;
    if (n->next)
        n->next = pair->next;
    pty_bitmap[pair->id / 8] &= ~(1 << (pair->id % 8));
    spin_unlock(&pty_global_lock);

    if (pts_node)
        vfs_iput(pts_node);
    if (ptmx_node)
        vfs_iput(ptmx_node);
    free(pair->bufferMaster);
    free(pair->bufferSlave);
    free(pair);
}

static void pts_ctrl_assign(pty_pair_t *pair) {
    if (!pair || !current_task)
        return;

    pair->ctrlSession = current_task->sid;
    pair->ctrlPgid = current_task->pgid;
    if (!pair->frontProcessGroup)
        pair->frontProcessGroup = pair->ctrlPgid;
}

static int pty_foreground_pgid_locked(pty_pair_t *pair) {
    if (!pair)
        return 0;

    return pair->frontProcessGroup ? pair->frontProcessGroup : pair->ctrlPgid;
}

static struct vfs_inode *
devpts_create_first_mounted_inode(pty_pair_t *pair,
                                  struct vfs_mount **out_mnt) {
    devpts_fs_info_t *fsi, *tmp;
    struct vfs_super_block *sb = NULL;
    struct vfs_mount *mnt = NULL;
    struct vfs_inode *inode;

    if (!pair || !out_mnt)
        return NULL;

    spin_lock(&pty_global_lock);
    llist_for_each(fsi, tmp, &devpts_superblocks, node) {
        if (!fsi->sb)
            continue;
        spin_lock(&fsi->sb->s_mount_lock);
        if (!llist_empty(&fsi->sb->s_mounts)) {
            mnt = list_entry(fsi->sb->s_mounts.next, struct vfs_mount,
                             mnt_sb_link);
            mnt = vfs_mntget(mnt);
            sb = fsi->sb;
            if (!mnt || !vfs_get_super(sb)) {
                if (mnt)
                    vfs_mntput(mnt);
                mnt = NULL;
                sb = NULL;
                spin_unlock(&fsi->sb->s_mount_lock);
                continue;
            }
            spin_unlock(&fsi->sb->s_mount_lock);
            break;
        }
        spin_unlock(&fsi->sb->s_mount_lock);
    }
    spin_unlock(&pty_global_lock);

    if (!sb || !mnt)
        return NULL;

    inode = devpts_new_inode(sb, S_IFCHR | 0620, pair);
    vfs_put_super(sb);
    if (!inode) {
        vfs_mntput(mnt);
        return NULL;
    }

    *out_mnt = mnt;
    return inode;
}

static int pty_open_peer_fd(pty_pair_t *pair, uint64_t flags) {
    static const uint64_t allowed_flags =
        O_ACCMODE_FLAGS | O_NOCTTY | O_NONBLOCK | O_CLOEXEC;
    struct vfs_file *file = NULL;
    struct vfs_path path = {0};
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    struct vfs_mount *mnt = NULL;
    struct vfs_qstr name;
    char name_buf[16];
    int ret;

    if (!pair || !current_task)
        return -EINVAL;
    if (flags & ~allowed_flags)
        return -EINVAL;

    spin_lock(&pair->lock);
    if (pair->locked) {
        spin_unlock(&pair->lock);
        return -EIO;
    }
    spin_unlock(&pair->lock);

    spin_lock(&pty_global_lock);
    inode = pair->pts_node ? vfs_igrab(pair->pts_node) : NULL;
    spin_unlock(&pty_global_lock);
    if (!inode)
        inode = devpts_create_first_mounted_inode(pair, &mnt);

    if (inode && !mnt && inode->i_sb) {
        spin_lock(&inode->i_sb->s_mount_lock);
        if (!llist_empty(&inode->i_sb->s_mounts)) {
            mnt = list_entry(inode->i_sb->s_mounts.next, struct vfs_mount,
                             mnt_sb_link);
            mnt = vfs_mntget(mnt);
        }
        spin_unlock(&inode->i_sb->s_mount_lock);
    }
    if (!inode || !mnt) {
        if (inode)
            vfs_iput(inode);
        if (mnt)
            vfs_mntput(mnt);
        return -ENODEV;
    }

    if (!inode->i_private) {
        vfs_iput(inode);
        vfs_mntput(mnt);
        return -ENODEV;
    }

    snprintf(name_buf, sizeof(name_buf), "%d", pair->id);
    vfs_qstr_make(&name, name_buf);
    dentry = vfs_d_alloc(inode->i_sb, inode->i_sb->s_root, &name);
    if (!dentry) {
        vfs_mntput(mnt);
        vfs_iput(inode);
        return -ENOMEM;
    }
    vfs_d_instantiate(dentry, inode);
    path.mnt = mnt;
    path.dentry = dentry;
    file = vfs_alloc_file(&path, O_RDWR | (unsigned int)(flags & O_NONBLOCK));
    if (!file) {
        vfs_dput(dentry);
        vfs_mntput(mnt);
        vfs_iput(inode);
        return -ENOMEM;
    }

    if (file->f_op && file->f_op->open) {
        ret = file->f_op->open(inode, file);
        if (ret < 0) {
            vfs_file_put(file);
            vfs_dput(dentry);
            vfs_mntput(mnt);
            vfs_iput(inode);
            return ret;
        }
    }

    ret = task_install_file(current_task, file,
                            (flags & O_CLOEXEC) ? FD_CLOEXEC : 0, 0);
    vfs_file_put(file);
    vfs_dput(dentry);
    vfs_mntput(mnt);
    vfs_iput(inode);
    return ret;
}

void pty_init() {
    if (pty_bitmap)
        return;
    pty_bitmap = calloc(PTY_MAX / 8, 1);
    first_pair.id = 0xffffffff;
}

static int ptmx_open_file(struct vfs_inode *inode, struct vfs_file *file) {
    int id;
    pty_pair_t *pair;

    if (!inode || !file)
        return -EINVAL;

    id = pty_bitmap_decide();
    if (id < 0)
        return -ENOSPC;

    pair = calloc(1, sizeof(*pair));
    if (!pair) {
        pty_bitmap_remove(id);
        return -ENOMEM;
    }

    spin_init(&pair->lock);
    llist_init_head(&pair->pts_nodes);
    pair->id = id;
    pair->bufferMaster = malloc(PTY_BUFF_SIZE);
    pair->bufferSlave = malloc(PTY_BUFF_SIZE);
    if (!pair->bufferMaster || !pair->bufferSlave) {
        free(pair->bufferMaster);
        free(pair->bufferSlave);
        free(pair);
        pty_bitmap_remove(id);
        return -ENOMEM;
    }

    pty_termios_default(&pair->term);
    pair->input_speed = 38400;
    pair->output_speed = 38400;
    pair->win.ws_row = 24;
    pair->win.ws_col = 80;
    pair->tty_kbmode = K_XLATE;
    pair->masterFds = 1;
    pair->ptmx_node = vfs_igrab(inode);

    spin_lock(&pty_global_lock);
    pty_pair_t *n = &first_pair;
    while (n->next)
        n = n->next;
    n->next = pair;
    spin_unlock(&pty_global_lock);

    if (pty_pair_install_slave_node(pair) < 0) {
        pty_pair_cleanup(pair);
        return -EIO;
    }

    file->private_data = pair;
    return 0;
}

static int ptmx_release_file(struct vfs_inode *inode, struct vfs_file *file) {
    pty_pair_t *pair = pty_pair_from_file(file);
    bool notify_hup = false;

    (void)inode;
    if (!pair)
        return 0;

    spin_lock(&pair->lock);
    if (pair->masterFds > 0)
        pair->masterFds--;
    notify_hup = pair->masterFds == 0;
    spin_unlock(&pair->lock);

    if (notify_hup)
        pty_notify_slaves(pair, EPOLLHUP | EPOLLRDHUP | EPOLLIN | EPOLLOUT |
                                    EPOLLRDNORM | EPOLLWRNORM);
    if (!pair->masterFds && !pair->slaveFds)
        pty_pair_cleanup(pair);
    file->private_data = NULL;
    return 0;
}

static size_t ptmx_data_avail(pty_pair_t *pair) { return pair->ptrMaster; }

static ssize_t ptmx_read(fd_t *fd, void *addr, size_t offset, size_t size) {
    pty_pair_t *pair = pty_pair_from_file(fd);
    (void)offset;
    if (!pair)
        return -EINVAL;
    if (!size)
        return 0;

    while (true) {
        spin_lock(&pair->lock);
        if (pair->packet_mode && pair->packet_status) {
            ((uint8_t *)addr)[0] = pair->packet_status;
            pair->packet_status = 0;
            spin_unlock(&pair->lock);
            return 1;
        }
        if (ptmx_data_avail(pair) > 0) {
            size_t header = pair->packet_mode ? 1 : 0;
            size_t to_copy = MIN(size - header, ptmx_data_avail(pair));
            if (header) {
                ((uint8_t *)addr)[0] = TIOCPKT_DATA;
            }
            if (to_copy > 0)
                memcpy((uint8_t *)addr + header, pair->bufferMaster, to_copy);
            size_t remaining = pair->ptrMaster - to_copy;
            if (remaining > 0)
                memmove(pair->bufferMaster, &pair->bufferMaster[to_copy],
                        remaining);
            pair->ptrMaster -= to_copy;
            pair->packet_data_pending =
                pair->packet_mode && pair->ptrMaster > 0;
            spin_unlock(&pair->lock);
            if (to_copy > 0)
                pty_notify_slaves(pair, EPOLLOUT | EPOLLWRNORM);
            return (ssize_t)(header + to_copy);
        }
        bool no_slave = (pair->slaveFds == 0);
        spin_unlock(&pair->lock);
        if (no_slave)
            return 0;
        if (fd_get_flags(fd) & O_NONBLOCK)
            return -EWOULDBLOCK;

        int reason = vfs_poll_wait_interruptible(
            fd, EPOLLIN | EPOLLRDNORM | EPOLLPRI | EPOLLHUP | EPOLLRDHUP);
        if (reason < 0)
            return reason;
    }
}

static ssize_t ptmx_write(fd_t *fd, const void *addr, size_t offset,
                          size_t limit) {
    pty_pair_t *pair = pty_pair_from_file(fd);
    (void)offset;
    if (!pair)
        return -EINVAL;

    while (true) {
        spin_lock(&pair->lock);
        if (pair->stop_master_output) {
            spin_unlock(&pair->lock);
            if (fd_get_flags(fd) & O_NONBLOCK)
                return -EWOULDBLOCK;

            if (task_signal_has_deliverable(current_task))
                return -EINTR;

            int reason = vfs_poll_wait_interruptible(
                fd, EPOLLOUT | EPOLLWRNORM | EPOLLHUP | EPOLLRDHUP);
            if (reason < 0)
                return reason;

            continue;
        }
        if (!pair->slaveFds) {
            spin_unlock(&pair->lock);
            return -EIO;
        }
        if (pair->ptrSlave < PTY_BUFF_SIZE) {
            const uint8_t *in = addr;
            size_t written = 0, echoed = 0, echo_start = pair->ptrSlave;
            for (size_t i = 0; i < limit; i++) {
                uint8_t ch = in[i];
                if (pair->term.c_lflag & ISIG) {
                    uint64_t pgid = pair->frontProcessGroup
                                        ? pair->frontProcessGroup
                                        : pair->ctrlPgid;
                    if (pgid) {
                        if (ch == pair->term.c_cc[VINTR]) {
                            send_process_group_signal(pgid, SIGINT);
                            written++;
                            continue;
                        }
                        if (ch == pair->term.c_cc[VQUIT]) {
                            send_process_group_signal(pgid, SIGQUIT);
                            written++;
                            continue;
                        }
                        if (ch == pair->term.c_cc[VSUSP]) {
                            send_process_group_signal(pgid, SIGTSTP);
                            written++;
                            continue;
                        }
                    }
                }
                if ((pair->term.c_iflag & ICRNL) && ch == '\r')
                    ch = '\n';
                if (pair->ptrSlave + 1 > PTY_BUFF_SIZE)
                    break;
                pair->bufferSlave[pair->ptrSlave++] = ch;
                echoed++;
                written++;
            }
            spin_unlock(&pair->lock);
            if ((pair->term.c_lflag & ICANON) && (pair->term.c_lflag & ECHO) &&
                echoed > 0)
                pts_write_inner(fd, &pair->bufferSlave[echo_start], echoed);
            if (written > 0)
                pty_notify_slaves(pair, EPOLLIN | EPOLLRDNORM);
            return (ssize_t)written;
        }
        spin_unlock(&pair->lock);
        if (fd_get_flags(fd) & O_NONBLOCK)
            return -EWOULDBLOCK;

        if (task_signal_has_deliverable(current_task))
            return -EINTR;

        int reason = vfs_poll_wait_interruptible(fd, EPOLLOUT | EPOLLWRNORM |
                                                         EPOLLHUP | EPOLLRDHUP);
        if (reason < 0)
            return reason;
    }
}

static long ptmx_ioctl(fd_t *fd, unsigned long request, unsigned long arg) {
    pty_pair_t *pair = pty_pair_from_file(fd);
    int ret = -ENOTTY;
    uint32_t notify_master = 0, notify_slave = 0;
    uint8_t packet_status = 0;
    size_t number = _IOC_NR(request);

    if (!pair)
        return -EINVAL;
    if ((request & 0xffffffffUL) == TIOCGPTPEER)
        return pty_open_peer_fd(pair, (uint64_t)arg);

    spin_lock(&pair->lock);
    switch (number) {
    case 0x31: {
        int lock = 0;
        if (!arg || copy_from_user(&lock, (const void *)arg, sizeof(lock)))
            ret = -EFAULT;
        else {
            pair->locked = lock != 0;
            ret = 0;
        }
        break;
    }
    case 0x30:
        ret = (!arg || copy_to_user((void *)arg, &pair->id, sizeof(int)))
                  ? -EFAULT
                  : 0;
        break;
    }

    if (ret == -ENOTTY) {
        switch (request & 0xffffffffU) {
        case TIOCGWINSZ:
            ret = (!arg || copy_to_user((void *)arg, &pair->win,
                                        sizeof(struct winsize)))
                      ? -EFAULT
                      : 0;
            break;
        case TIOCSWINSZ:
            ret = (!arg || copy_from_user(&pair->win, (const void *)arg,
                                          sizeof(struct winsize)))
                      ? -EFAULT
                      : 0;
            break;
        case TIOCSCTTY:
            pts_ctrl_assign(pair);
            ret = 0;
            break;
        case TCGETS:
            ret = (!arg ||
                   copy_to_user((void *)arg, &pair->term, sizeof(termios)))
                      ? -EFAULT
                      : 0;
            break;
        case TCGETS2: {
            struct termios2 term2;
            pty_get_termios2_locked(pair, &term2);
            ret = (!arg || copy_to_user((void *)arg, &term2, sizeof(term2)))
                      ? -EFAULT
                      : 0;
            break;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            struct termios old_term = pair->term;
            ret = (!arg || copy_from_user(&pair->term, (const void *)arg,
                                          sizeof(termios)))
                      ? -EFAULT
                      : 0;
            if (!ret) {
                pty_packet_termios_changed_locked(pair, &old_term,
                                                  &notify_master);
                if ((request & 0xffffffffU) == TCSETSF)
                    ret = pty_tcflush_locked(pair, TCIFLUSH, &notify_master,
                                             &notify_slave, &packet_status);
            }
            break;
        }
        case TCSETS2:
        case TCSETSW2:
        case TCSETSF2: {
            struct termios old_term = pair->term;
            struct termios2 term2;
            ret = (!arg ||
                   copy_from_user(&term2, (const void *)arg, sizeof(term2)))
                      ? -EFAULT
                      : 0;
            if (!ret) {
                pty_set_termios2_locked(pair, &term2);
                pty_packet_termios_changed_locked(pair, &old_term,
                                                  &notify_master);
                if ((request & 0xffffffffU) == TCSETSF2)
                    ret = pty_tcflush_locked(pair, TCIFLUSH, &notify_master,
                                             &notify_slave, &packet_status);
            }
            break;
        }
        case TCXONC:
            ret = pty_tcxonc_locked(pair, false, arg, &notify_master,
                                    &notify_slave, &packet_status);
            break;
        case TCFLSH:
            ret = pty_tcflush_locked(pair, (int)arg, &notify_master,
                                     &notify_slave, &packet_status);
            break;
        case TIOCPKT: {
            int enabled = 0;
            if (!arg ||
                copy_from_user(&enabled, (const void *)arg, sizeof(enabled))) {
                ret = -EFAULT;
                break;
            }
            pair->packet_mode = enabled != 0;
            pair->packet_status = 0;
            pair->packet_data_pending =
                pair->packet_mode && pair->ptrMaster > 0;
            ret = 0;
            break;
        }
        case TIOCGPKT: {
            int enabled = pair->packet_mode ? 1 : 0;
            ret = (!arg || copy_to_user((void *)arg, &enabled, sizeof(enabled)))
                      ? -EFAULT
                      : 0;
            break;
        }
        case FIONREAD: {
            int available = 0;
            if (!pair->packet_mode ||
                (!pair->packet_status && !pair->packet_data_pending)) {
                available = (int)ptmx_data_avail(pair);
            }
            ret = (!arg ||
                   copy_to_user((void *)arg, &available, sizeof(available)))
                      ? -EFAULT
                      : 0;
            break;
        }
        case TIOCOUTQ: {
            int queued = 0;
            ret = (!arg || copy_to_user((void *)arg, &queued, sizeof(queued)))
                      ? -EFAULT
                      : 0;
            break;
        }
        case TCSBRK:
        case TCSBRKP:
            ret = 0;
            break;
        case TIOCGPGRP:
            if (!arg) {
                ret = -EFAULT;
                break;
            }
            int foreground_pgid = pty_foreground_pgid_locked(pair);
            ret = copy_to_user((void *)arg, &foreground_pgid,
                               sizeof(foreground_pgid))
                      ? -EFAULT
                      : 0;
            break;
        case TIOCSPGRP:
            ret = copy_from_user(&pair->frontProcessGroup, (const void *)arg,
                                 sizeof(int))
                      ? -EFAULT
                      : 0;
            break;
        case TIOCNOTTY:
        case VT_ACTIVATE:
        case VT_WAITACTIVE:
            ret = 0;
            break;
        default:
            spin_unlock(&pair->lock);
            return pts_ioctl(pair, request, (void *)arg);
        }
    }
    if (!ret)
        pty_packet_queue_locked(pair, packet_status, &notify_master);
    spin_unlock(&pair->lock);

    if (!ret)
        pty_notify_pair(pair, notify_master, notify_slave);
    return ret;
}

static __poll_t ptmx_poll(fd_t *file, struct vfs_poll_table *pt) {
    pty_pair_t *pair = pty_pair_from_file(file);
    int revents = 0;
    (void)pt;
    if (!pair)
        return EPOLLNVAL;

    spin_lock(&pair->lock);
    if (pair->packet_mode && pair->packet_status)
        revents |= EPOLLIN | EPOLLRDNORM | EPOLLPRI;
    if (ptmx_data_avail(pair) > 0)
        revents |= EPOLLIN | EPOLLRDNORM;
    if (pair->ptrSlave < PTY_BUFF_SIZE)
        revents |= EPOLLOUT | EPOLLWRNORM;
    if (!pair->slaveFds)
        revents |= EPOLLHUP | EPOLLRDHUP;
    spin_unlock(&pair->lock);
    return revents;
}

static int pts_open_file(struct vfs_inode *inode, struct vfs_file *file) {
    pty_pair_t *pair = inode ? (pty_pair_t *)inode->i_private : NULL;

    if (!pair || !file)
        return -EINVAL;
    spin_lock(&pair->lock);
    if (pair->locked) {
        spin_unlock(&pair->lock);
        return -EIO;
    }
    pair->slaveFds++;
    file->private_data = pair;
    spin_unlock(&pair->lock);
    pty_notify_master(pair, EPOLLOUT | EPOLLWRNORM);
    return 0;
}

static int pts_release_file(struct vfs_inode *inode, struct vfs_file *file) {
    pty_pair_t *pair = pty_pair_from_file(file);
    bool notify_hup = false;
    (void)inode;
    if (!pair)
        return 0;

    spin_lock(&pair->lock);
    if (pair->slaveFds > 0)
        pair->slaveFds--;
    notify_hup = pair->slaveFds == 0;
    spin_unlock(&pair->lock);

    if (notify_hup)
        pty_notify_master(pair, EPOLLHUP | EPOLLRDHUP | EPOLLIN | EPOLLOUT |
                                    EPOLLRDNORM | EPOLLWRNORM);
    if (!pair->masterFds && !pair->slaveFds)
        pty_pair_cleanup(pair);
    file->private_data = NULL;
    return 0;
}

static size_t pts_data_avail(pty_pair_t *pair) {
    if (!(pair->term.c_lflag & ICANON))
        return pair->ptrSlave;
    for (size_t i = 0; i < (size_t)pair->ptrSlave; i++) {
        if (pair->bufferSlave[i] == '\n' ||
            pair->bufferSlave[i] == pair->term.c_cc[VEOF] ||
            pair->bufferSlave[i] == pair->term.c_cc[VEOL] ||
            pair->bufferSlave[i] == pair->term.c_cc[VEOL2])
            return i + 1;
    }
    return 0;
}

static ssize_t pts_read(fd_t *fd, void *out, size_t offset, size_t limit) {
    pty_pair_t *pair = pty_pair_from_file(fd);
    (void)offset;
    if (!pair)
        return -EINVAL;

    while (true) {
        spin_lock(&pair->lock);
        if (pts_data_avail(pair) > 0) {
            size_t to_copy = MIN(limit, pts_data_avail(pair));
            memcpy(out, pair->bufferSlave, to_copy);
            size_t remaining = pair->ptrSlave - to_copy;
            if (remaining > 0)
                memmove(pair->bufferSlave, &pair->bufferSlave[to_copy],
                        remaining);
            pair->ptrSlave -= to_copy;
            spin_unlock(&pair->lock);
            if (to_copy > 0)
                pty_notify_master(pair, EPOLLOUT | EPOLLWRNORM);
            return (ssize_t)to_copy;
        }
        bool no_master = (pair->masterFds == 0);
        spin_unlock(&pair->lock);
        if (no_master)
            return 0;
        if (fd_get_flags(fd) & O_NONBLOCK)
            return -EWOULDBLOCK;

        int reason = vfs_poll_wait_interruptible(fd, EPOLLIN | EPOLLRDNORM |
                                                         EPOLLHUP | EPOLLRDHUP);
        if (reason < 0)
            return reason;
    }
}

ssize_t pts_write_inner(fd_t *fd, uint8_t *in, size_t limit) {
    pty_pair_t *pair = pty_pair_from_file(fd);
    if (!pair)
        return -EINVAL;

    while (true) {
        spin_lock(&pair->lock);
        if (pair->stop_slave_output) {
            spin_unlock(&pair->lock);
            if (fd_get_flags(fd) & O_NONBLOCK)
                return -EWOULDBLOCK;

            if (task_signal_has_deliverable(current_task))
                return -EINTR;

            int reason = vfs_poll_wait_interruptible(
                fd, EPOLLOUT | EPOLLWRNORM | EPOLLHUP | EPOLLRDHUP);
            if (reason < 0)
                return reason;

            continue;
        }
        if (!pair->masterFds) {
            spin_unlock(&pair->lock);
            return -EIO;
        }
        if (pair->ptrMaster < PTY_BUFF_SIZE) {
            size_t old_ptr_master = pair->ptrMaster;
            size_t written = 0;
            bool translate =
                (pair->term.c_oflag & OPOST) && (pair->term.c_oflag & ONLCR);
            for (size_t i = 0; i < limit; i++) {
                uint8_t ch = in[i];
                if (translate && ch == '\n') {
                    if (pair->ptrMaster + 2 > PTY_BUFF_SIZE)
                        break;
                    pair->bufferMaster[pair->ptrMaster++] = '\r';
                    pair->bufferMaster[pair->ptrMaster++] = '\n';
                } else {
                    if (pair->ptrMaster + 1 > PTY_BUFF_SIZE)
                        break;
                    pair->bufferMaster[pair->ptrMaster++] = ch;
                }
                written++;
            }
            if (written > 0)
                pty_packet_mark_data_locked(pair, old_ptr_master);
            spin_unlock(&pair->lock);
            if (written > 0)
                pty_notify_master(pair, EPOLLIN | EPOLLRDNORM |
                                            (pair->packet_mode ? EPOLLPRI : 0));
            return (ssize_t)written;
        }
        spin_unlock(&pair->lock);
        if (fd_get_flags(fd) & O_NONBLOCK)
            return -EWOULDBLOCK;

        if (task_signal_has_deliverable(current_task))
            return -EINTR;

        int reason = vfs_poll_wait_interruptible(fd, EPOLLOUT | EPOLLWRNORM |
                                                         EPOLLHUP | EPOLLRDHUP);
        if (reason < 0)
            return reason;
    }
}

static ssize_t pts_write(fd_t *fd, const void *in, size_t offset,
                         size_t limit) {
    ssize_t ret = 0;
    (void)offset;
    size_t chunks = limit / PTY_BUFF_SIZE;
    size_t remainder = limit % PTY_BUFF_SIZE;
    const uint8_t *buf = in;

    for (size_t i = 0; i < chunks; i++) {
        size_t cycle = 0;
        while (cycle < PTY_BUFF_SIZE) {
            ssize_t r =
                pts_write_inner(fd, (uint8_t *)buf + i * PTY_BUFF_SIZE + cycle,
                                PTY_BUFF_SIZE - cycle);
            if (r < 0)
                return ret ? ret : r;
            cycle += (size_t)r;
        }
        ret += (ssize_t)cycle;
    }

    if (remainder) {
        size_t cycle = 0;
        while (cycle < remainder) {
            ssize_t r = pts_write_inner(
                fd, (uint8_t *)buf + chunks * PTY_BUFF_SIZE + cycle,
                remainder - cycle);
            if (r < 0)
                return ret ? ret : r;
            cycle += (size_t)r;
        }
        ret += (ssize_t)cycle;
    }
    return ret;
}

int pts_ioctl(pty_pair_t *pair, uint64_t request, void *arg) {
    int ret = -ENOTTY;
    uint32_t notify_master = 0, notify_slave = 0;
    uint8_t packet_status = 0;

    if (!pair)
        return -EINVAL;

    spin_lock(&pair->lock);
    switch (request) {
    case TIOCGWINSZ:
        ret = (!arg || copy_to_user(arg, &pair->win, sizeof(struct winsize)))
                  ? -EFAULT
                  : 0;
        break;
    case TIOCSWINSZ:
        ret = (!arg || copy_from_user(&pair->win, arg, sizeof(struct winsize)))
                  ? -EFAULT
                  : 0;
        break;
    case TIOCSCTTY:
        pts_ctrl_assign(pair);
        ret = 0;
        break;
    case TCGETS:
        ret = (!arg || copy_to_user(arg, &pair->term, sizeof(termios)))
                  ? -EFAULT
                  : 0;
        break;
    case TCGETS2: {
        struct termios2 term2;
        pty_get_termios2_locked(pair, &term2);
        ret = (!arg || copy_to_user(arg, &term2, sizeof(term2))) ? -EFAULT : 0;
        break;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios old_term = pair->term;
        ret = (!arg || copy_from_user(&pair->term, arg, sizeof(termios)))
                  ? -EFAULT
                  : 0;
        if (!ret) {
            pty_packet_termios_changed_locked(pair, &old_term, &notify_master);
            if (request == TCSETSF)
                ret = pty_tcflush_locked(pair, TCIFLUSH, &notify_master,
                                         &notify_slave, &packet_status);
        }
        break;
    }
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2: {
        struct termios old_term = pair->term;
        struct termios2 term2;
        ret =
            (!arg || copy_from_user(&term2, arg, sizeof(term2))) ? -EFAULT : 0;
        if (!ret) {
            pty_set_termios2_locked(pair, &term2);
            pty_packet_termios_changed_locked(pair, &old_term, &notify_master);
            if (request == TCSETSF2)
                ret = pty_tcflush_locked(pair, TCIFLUSH, &notify_master,
                                         &notify_slave, &packet_status);
        }
        break;
    }
    case TCXONC:
        ret = pty_tcxonc_locked(pair, false, (uintptr_t)arg, &notify_master,
                                &notify_slave, &packet_status);
        break;
    case FIONREAD: {
        int available = (int)pts_data_avail(pair);
        ret = (!arg || copy_to_user(arg, &available, sizeof(available)))
                  ? -EFAULT
                  : 0;
        break;
    }
    case TIOCOUTQ: {
        int queued = 0;
        ret =
            (!arg || copy_to_user(arg, &queued, sizeof(queued))) ? -EFAULT : 0;
        break;
    }
    case TCFLSH:
        ret = pty_tcflush_locked(pair, (int)(uintptr_t)arg, &notify_master,
                                 &notify_slave, &packet_status);
        break;
    case TCSBRK:
    case TCSBRKP:
        ret = 0;
        break;
    case TIOCGPGRP:
        if (!arg) {
            ret = -EFAULT;
            break;
        }
        int foreground_pgid = pty_foreground_pgid_locked(pair);
        ret = copy_to_user(arg, &foreground_pgid, sizeof(foreground_pgid))
                  ? -EFAULT
                  : 0;
        break;
    case TIOCSPGRP:
        ret = copy_from_user(&pair->frontProcessGroup, arg, sizeof(int))
                  ? -EFAULT
                  : 0;
        break;
    case TIOCNOTTY:
    case VT_ACTIVATE:
    case VT_WAITACTIVE:
        ret = 0;
        break;
    default:
        printk("pts_ioctl: Unsupported request %#010lx\n", request);
        break;
    }
    if (!ret)
        pty_packet_queue_locked(pair, packet_status, &notify_master);
    spin_unlock(&pair->lock);

    if (!ret)
        pty_notify_pair(pair, notify_master, notify_slave);
    return ret;
}

static long pts_ioctl_file(fd_t *fd, unsigned long request, unsigned long arg) {
    return pts_ioctl(pty_pair_from_file(fd), request, (void *)arg);
}

static ssize_t ptmx_read_file(struct vfs_file *fd, void *buf, size_t count,
                              loff_t *ppos) {
    (void)ppos;
    return ptmx_read(fd, buf, 0, count);
}

static ssize_t ptmx_write_file(struct vfs_file *fd, const void *buf,
                               size_t count, loff_t *ppos) {
    (void)ppos;
    return ptmx_write(fd, buf, 0, count);
}

static ssize_t pts_read_file(struct vfs_file *fd, void *buf, size_t count,
                             loff_t *ppos) {
    (void)ppos;
    return pts_read(fd, buf, 0, count);
}

static ssize_t pts_write_file(struct vfs_file *fd, const void *buf,
                              size_t count, loff_t *ppos) {
    (void)ppos;
    return pts_write(fd, buf, 0, count);
}

static __poll_t pts_poll(fd_t *file, struct vfs_poll_table *pt) {
    pty_pair_t *pair = pty_pair_from_file(file);
    int revents = 0;
    (void)pt;
    if (!pair)
        return EPOLLNVAL;

    spin_lock(&pair->lock);
    if (pts_data_avail(pair) > 0)
        revents |= EPOLLIN | EPOLLRDNORM;
    if (pair->ptrMaster < PTY_BUFF_SIZE)
        revents |= EPOLLOUT | EPOLLWRNORM;
    if (!pair->masterFds)
        revents |= EPOLLHUP | EPOLLRDHUP;
    spin_unlock(&pair->lock);
    return revents;
}

static loff_t pty_llseek(struct vfs_file *file, loff_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static const struct vfs_file_operations ptmx_file_ops = {
    .llseek = pty_llseek,
    .read = ptmx_read_file,
    .write = ptmx_write_file,
    .unlocked_ioctl = ptmx_ioctl,
    .poll = ptmx_poll,
    .open = ptmx_open_file,
    .release = ptmx_release_file,
};

static const struct vfs_file_operations pts_file_ops = {
    .llseek = pty_llseek,
    .read = pts_read_file,
    .write = pts_write_file,
    .unlocked_ioctl = pts_ioctl_file,
    .poll = pts_poll,
    .open = pts_open_file,
    .release = pts_release_file,
};

void ptmx_init() {
    vfs_node_t *ptmx;

    (void)vfs_mknodat(AT_FDCWD, "/dev/ptmx", S_IFCHR | 0666, (5U << 8) | 2U,
                      true);
    ptmx = pty_lookup_inode_path("/dev/ptmx");
    if (!ptmx)
        return;
    ptmx->i_fop = &ptmx_file_ops;
    vfs_iput(ptmx);
}

void pts_init() {
    pty_init();
    (void)vfs_register_filesystem(&devpts_fs_type);
    if (first_pair.id == 0) {
        first_pair.id = 0xffffffff;
    }
}

void pts_repopulate_nodes() {
    pty_pair_t *pairs[PTY_MAX];
    size_t count = 0;
    pty_pair_t *pair;

    if (!pty_bitmap)
        return;

    spin_lock(&pty_global_lock);
    for (pair = first_pair.next; pair && count < PTY_MAX; pair = pair->next)
        pairs[count++] = pair;
    spin_unlock(&pty_global_lock);

    for (size_t i = 0; i < count; i++)
        (void)pty_pair_install_slave_node(pairs[i]);
}

static struct pty_pair *devpts_find_pair_by_id(int id) {
    pty_pair_t *pair;

    spin_lock(&pty_global_lock);
    for (pair = first_pair.next; pair; pair = pair->next) {
        if (pair->id == id)
            break;
    }
    spin_unlock(&pty_global_lock);
    return pair;
}

static int devpts_parse_id(const char *name, int *id_out) {
    int id = 0;

    if (!name || !name[0] || !id_out)
        return -EINVAL;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9')
            return -ENOENT;
        if (id > (PTY_MAX - 1) / 10)
            return -ENOENT;
        id = id * 10 + (*p - '0');
        if (id >= PTY_MAX)
            return -ENOENT;
    }
    *id_out = id;
    return 0;
}

static struct vfs_dentry *devpts_lookup(struct vfs_inode *dir,
                                        struct vfs_dentry *dentry,
                                        unsigned int flags) {
    struct vfs_inode *inode;
    int id;
    int ret;

    (void)flags;
    if (!dir || !dentry)
        return ERR_PTR(-EINVAL);

    ret = devpts_parse_id(dentry->d_name.name, &id);
    if (ret < 0) {
        vfs_d_instantiate(dentry, NULL);
        return dentry;
    }

    if (!devpts_find_pair_by_id(id)) {
        vfs_d_instantiate(dentry, NULL);
        return dentry;
    }

    inode = devpts_new_slave_inode_for_id(dir->i_sb, id);
    if (!inode) {
        if (devpts_find_pair_by_id(id))
            return ERR_PTR(-ENOMEM);
        vfs_d_instantiate(dentry, NULL);
        return dentry;
    }

    vfs_d_instantiate(dentry, inode);
    vfs_iput(inode);
    return dentry;
}

static int devpts_permission(struct vfs_inode *inode, int mask) {
    return vfs_inode_permission(inode, mask);
}

static int devpts_getattr(const struct vfs_path *path, struct vfs_kstat *stat,
                          uint32_t request_mask, unsigned int flags) {
    (void)request_mask;
    (void)flags;
    vfs_fill_generic_kstat(path, stat);
    return 0;
}

static int devpts_iterate_shared(struct vfs_file *file,
                                 struct vfs_dir_context *ctx) {
    int ids[PTY_MAX];
    size_t count = 0;
    pty_pair_t *pair;
    loff_t index = 0;

    if (!file || !file->f_inode || !ctx || !S_ISDIR(file->f_inode->i_mode))
        return -ENOTDIR;

    spin_lock(&pty_global_lock);
    for (pair = first_pair.next; pair && count < PTY_MAX; pair = pair->next)
        ids[count++] = pair->id;
    spin_unlock(&pty_global_lock);

    for (size_t i = 0; i < count; i++) {
        char name[16];

        if (index++ < ctx->pos)
            continue;
        snprintf(name, sizeof(name), "%d", ids[i]);
        if (ctx->actor(ctx, name, (int)strlen(name), index, (ino64_t)ids[i] + 3,
                       DT_CHR)) {
            break;
        }
        ctx->pos = index;
    }

    file->f_pos = ctx->pos;
    return 0;
}

static int devpts_open_file(struct vfs_inode *inode, struct vfs_file *file) {
    if (!inode || !file)
        return -EINVAL;
    file->f_op = inode->i_fop;
    return 0;
}

static struct vfs_inode *devpts_alloc_inode(struct vfs_super_block *sb) {
    devpts_inode_info_t *info = calloc(1, sizeof(*info));

    (void)sb;
    return info ? &info->vfs_inode : NULL;
}

static void devpts_destroy_inode(struct vfs_inode *inode) {
    free(devpts_i(inode));
}

static void devpts_evict_inode(struct vfs_inode *inode) {
    devpts_inode_info_t *info = devpts_i(inode);
    pty_pair_t *pair;

    if (!info)
        return;

    pair = info->pair;
    if (pair) {
        spin_lock(&pty_global_lock);
        if (!llist_empty(&info->pair_node))
            llist_delete(&info->pair_node);
        if (pair->pts_node == inode) {
            pair->pts_node = NULL;
            if (!llist_empty(&pair->pts_nodes)) {
                devpts_inode_info_t *next = list_entry(
                    pair->pts_nodes.next, devpts_inode_info_t, pair_node);
                pair->pts_node = vfs_igrab(&next->vfs_inode);
            }
        }
        spin_unlock(&pty_global_lock);
    }
}

static int devpts_d_revalidate(struct vfs_dentry *dentry, unsigned int flags) {
    devpts_inode_info_t *info;
    pty_pair_t *pair;
    int id;

    (void)flags;
    if (!dentry)
        return 0;
    if (dentry->d_flags & VFS_DENTRY_ROOT)
        return 1;
    if (devpts_parse_id(dentry->d_name.name, &id) < 0)
        return dentry->d_inode ? 0 : 1;

    pair = devpts_find_pair_by_id(id);
    if (!dentry->d_inode)
        return pair ? 0 : 1;

    info = devpts_i(dentry->d_inode);
    return info && info->pair == pair;
}

static int devpts_get_tree(struct vfs_fs_context *fc) {
    struct vfs_super_block *sb;
    devpts_fs_info_t *fsi;
    struct vfs_inode *root_inode;
    struct vfs_dentry *root_dentry;
    struct vfs_qstr root_name = {.name = "", .len = 0, .hash = 0};

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
    fsi->sb = sb;
    llist_init_head(&fsi->node);

    sb->s_op = &devpts_super_ops;
    sb->s_d_op = &devpts_dentry_ops;
    sb->s_type = &devpts_fs_type;
    sb->s_magic = 0x1cd1ULL;
    sb->s_fs_info = fsi;

    root_inode = devpts_new_inode(sb, S_IFDIR | 0755, NULL);
    if (!root_inode) {
        free(fsi);
        sb->s_fs_info = NULL;
        vfs_put_super(sb);
        return -ENOMEM;
    }

    root_dentry = vfs_d_alloc(sb, NULL, &root_name);
    if (!root_dentry) {
        vfs_iput(root_inode);
        free(fsi);
        sb->s_fs_info = NULL;
        vfs_put_super(sb);
        return -ENOMEM;
    }

    vfs_d_instantiate(root_dentry, root_inode);
    sb->s_root = root_dentry;
    fc->sb = sb;
    vfs_iput(root_inode);

    spin_lock(&pty_global_lock);
    llist_append(&devpts_superblocks, &fsi->node);
    spin_unlock(&pty_global_lock);
    return 0;
}

static void devpts_put_super(struct vfs_super_block *sb) {
    devpts_fs_info_t *fsi = sb ? (devpts_fs_info_t *)sb->s_fs_info : NULL;

    if (!fsi)
        return;
    spin_lock(&pty_global_lock);
    if (!llist_empty(&fsi->node))
        llist_delete(&fsi->node);
    spin_unlock(&pty_global_lock);
    free(fsi);
    sb->s_fs_info = NULL;
}

static const struct vfs_super_operations devpts_super_ops = {
    .alloc_inode = devpts_alloc_inode,
    .destroy_inode = devpts_destroy_inode,
    .evict_inode = devpts_evict_inode,
    .put_super = devpts_put_super,
};

static const struct vfs_inode_operations devpts_inode_ops = {
    .lookup = devpts_lookup,
    .permission = devpts_permission,
    .getattr = devpts_getattr,
};

static const struct vfs_dentry_operations devpts_dentry_ops = {
    .d_revalidate = devpts_d_revalidate,
};

static const struct vfs_file_operations devpts_dir_file_ops = {
    .llseek = pty_llseek,
    .iterate_shared = devpts_iterate_shared,
    .open = devpts_open_file,
};

static struct vfs_file_system_type devpts_fs_type = {
    .name = "devpts",
    .fs_flags = VFS_FS_VIRTUAL,
    .get_tree = devpts_get_tree,
};
