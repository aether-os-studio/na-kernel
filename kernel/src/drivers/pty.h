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
    // above not used by ptyIsAssigned() since they aren't cleared

    int frontProcessGroup; // for job control

    int id;
    bool locked; // by default unlocked (hence 0)
} pty_pair_t;

void pty_init();
void ptmx_init();
void pts_init();
void pts_repopulate_nodes();
