#include <libs/klibc.h>
#include <fs/fs_syscall.h>
#include <fs/dev.h>
#include <libs/termios.h>

#define PTY_MAX 256
#define PTY_BUFF_SIZE (256 * 1024)

typedef struct pty_pair {
    vfs_node_t *ptmx_node;
    vfs_node_t *pts_node;
    struct llist_header pts_nodes;

    struct pty_pair *next;

    spinlock_t lock;

    int masterFds;
    int slaveFds;
    int active_releases;
    bool cleanup_started;

    termios term;
    uint32_t input_speed;
    uint32_t output_speed;
    struct winsize win;
    uint8_t *bufferMaster;
    uint8_t *bufferSlave;

    int ptrMaster;
    int ptrSlave;

    bool stop_master_output;
    bool stop_slave_output;
    bool packet_mode;
    bool packet_data_pending;
    uint8_t packet_status;

    int tty_kbmode;
    struct vt_mode vt_mode;

    // controlling stuff
    int ctrlSession;
    int ctrlPgid;

    int frontProcessGroup; // for job control

    /* Inode for a dynamic /dev/tty alias when this PTY is controlling. */
    vfs_node_t *ctrl_node;

    int id;
    bool locked; // by default unlocked (hence 0)

    uid32_t slave_uid;
    gid32_t slave_gid;
    umode_t slave_mode;
} pty_pair_t;

void pty_init();
void ptmx_init();
void pts_init();
void pts_repopulate_nodes();
ssize_t ptmx_device_open(void *data, void *arg);
ssize_t ptmx_device_close(void *data, void *arg);
ssize_t ptmx_device_ioctl(void *data, ssize_t request, ssize_t arg, fd_t *fd);
ssize_t ptmx_device_poll(void *data, int events, fd_t *fd);
ssize_t ptmx_device_read(void *data, void *buf, uint64_t offset, size_t size,
                         fd_t *fd);
ssize_t ptmx_device_write(void *data, void *buf, uint64_t offset, size_t size,
                          fd_t *fd);

pty_pair_t *pty_lookup_session_by_sid(uint64_t sid);
ssize_t pty_session_read(pty_pair_t *pair, fd_t *fd, void *buf, size_t count);
ssize_t pty_session_write(pty_pair_t *pair, fd_t *fd, const void *buf,
                          size_t count);
int pty_session_poll(pty_pair_t *pair, struct vfs_poll_table *pt);
void pty_session_bind_devnode(pty_pair_t *pair, vfs_node_t *node);
int pts_ioctl(pty_pair_t *pair, uint64_t request, void *arg);
