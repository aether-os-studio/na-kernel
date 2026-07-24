#include <libs/klibc.h>
#include <libs/llist.h>
#include <drivers/bus/bus.h>
#include <libs/termios.h>
#include <task/wait.h>

enum tty_device_type {
    TTY_DEVICE_SERIAL = 0, // 串口设备
    TTY_DEVICE_GRAPHI = 1, // 图形设备
};

typedef struct tty_virtual_device tty_device_t;
typedef struct tty_session tty_t;
typedef struct vfs_file fd_t;

typedef struct tty_device_ops {
    size_t (*write)(tty_device_t *device, const char *buf, size_t count);
    size_t (*read)(tty_device_t *device, char *buf, size_t count);
    void (*flush)(tty_device_t *res);
    int (*ioctl)(tty_device_t *device, uint32_t cmd, uint32_t arg);
} tty_device_ops_t;

struct tty_graphics_ {
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

struct tty_serial_ {
    uint16_t port;
};

typedef struct tty_virtual_device { // TTY 设备
    enum tty_device_type type;
    tty_device_ops_t ops; // 图形设备不具备 read write 操作
    void *private_data;   // 实际设备
    char name[32];

    struct llist_header node;
} tty_device_t;

typedef struct tty_session_ops {
    size_t (*write)(tty_t *device, const char *buf, size_t count);
    ssize_t (*read)(tty_t *device, char *buf, size_t count, fd_t *fd);
    void (*flush)(tty_t *res);
    int (*ioctl)(tty_t *device, uint32_t cmd, uint64_t arg);
    int (*poll)(tty_t *device, int events);
} tty_session_ops_t;

#define TTY_POLL_NODE_LIMIT 32

typedef struct tty_session { // 一个 TTY 会话
    void *terminal;
    struct termios termios;
    struct vt_mode current_vt_mode;
    int tty_kbmode;
    int tty_mode;
    uint64_t at_session_id;
    uint64_t at_process_group_id;
    char input_buf[1024];
    uint16_t input_head;
    uint16_t input_tail;
    uint16_t input_count;
    char canon_buf[1024];
    uint16_t canon_count;
    spinlock_t input_lock;
    wait_queue_head_t input_wait;
    bool key_shift;
    bool key_ctrl;
    bool key_alt;
    bool key_capslock;
    tty_session_ops_t ops;
    tty_device_t *device; // 会话所属的TTY设备
    vfs_node_t *poll_nodes[TTY_POLL_NODE_LIMIT];
    size_t poll_node_count;
    size_t poll_node_cursor;
    struct llist_header node;
} tty_t;

extern tty_t *kernel_session;

int tty_ioctl(void *dev, int cmd, void *args);
int tty_poll(void *dev, int events, fd_t *fd);
int tty_read(void *dev, void *buf, uint64_t offset, size_t size, fd_t *fd);
int tty_write(void *dev, const void *buf, uint64_t offset, size_t size,
              fd_t *fd);

tty_device_t *get_tty_device(const char *name);
tty_device_t *alloc_tty_device(enum tty_device_type type);
uint64_t register_tty_device(tty_device_t *device);
uint64_t delete_tty_device(tty_device_t *device);
void tty_init();
void tty_init_session();
void tty_init_session_serial();
void tty_bind_devnode(tty_t *tty, vfs_node_t *node);
void tty_notify_input_ready(tty_t *tty);
void tty_register_session(tty_t *tty);
tty_t *tty_lookup_session_by_sid(uint64_t sid);
void tty_session_attach_current(tty_t *tty);
void tty_session_detach_current(tty_t *tty);
int tty_input_available(tty_t *tty);
ssize_t tty_input_read(tty_t *tty, char *buf, size_t count, fd_t *fd);
int tty_input_poll(tty_t *tty, int events);
void tty_input_flush(tty_t *tty);
int tty_vt_activate(unsigned int vtnr);
int tty_vt_get_state(struct vt_state *state);
void tty_sysfs_register(uint64_t dev, const char *name);
void tty_input_event(dev_input_event_t *event, uint16_t type, uint16_t code,
                     int32_t value);
