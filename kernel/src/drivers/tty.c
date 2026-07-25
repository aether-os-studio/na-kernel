#include <drivers/tty.h>
#include <dev/device.h>
#include <mm/mm.h>
#include <boot/boot.h>
#include <task/task.h>
#include <task/signal.h>
#include <fs/vfs/fcntl.h>
#include <fs/sys.h>

DEFINE_LLIST(tty_device_list);
DEFINE_LLIST(tty_session_list);
tty_t *kernel_session = NULL; // 内核会话

static spinlock_t tty_vt_lock = SPIN_INIT;
static uint64_t tty_vt_present;
static unsigned int tty_active_vt = 1;
static unsigned int tty_pending_vt;
static vfs_node_t *tty_active_node;
static tty_t *tty_vts[64];
static tty_t *tty0_proxy;
static wait_queue_head_t tty_vt_wait;

extern void send_process_group_signal(int pgid, int sig);

#define TTY_INPUT_BUF_SIZE 1024

static inline bool tty_bitmap_test(const uint8_t *bitmap, size_t bit) {
    return bitmap && (bitmap[bit / 8] & (1u << (bit % 8))) != 0;
}

static bool tty_input_enqueue_byte(tty_t *tty, char c) {
    if (!tty)
        return false;

    if (tty->input_count >= TTY_INPUT_BUF_SIZE) {
        tty->input_head = (tty->input_head + 1) % TTY_INPUT_BUF_SIZE;
        tty->input_count--;
    }

    tty->input_buf[tty->input_tail] = c;
    tty->input_tail = (tty->input_tail + 1) % TTY_INPUT_BUF_SIZE;
    tty->input_count++;
    return true;
}

static bool tty_input_dequeue_byte(tty_t *tty, char *c) {
    if (!tty || !c || tty->input_count == 0)
        return false;

    *c = tty->input_buf[tty->input_head];
    tty->input_head = (tty->input_head + 1) % TTY_INPUT_BUF_SIZE;
    tty->input_count--;
    return true;
}

void tty_bind_devnode(tty_t *tty, vfs_node_t *node) {
    vfs_node_t *new_node;

    if (!tty || !node)
        return;

    for (size_t i = 0; i < tty->poll_node_count; i++) {
        if (tty->poll_nodes[i] == node)
            return;
    }

    new_node = vfs_igrab(node);
    if (!new_node)
        return;

    if (tty->poll_node_count < TTY_POLL_NODE_LIMIT) {
        tty->poll_nodes[tty->poll_node_count++] = new_node;
        return;
    }

    size_t index = tty->poll_node_cursor++ % TTY_POLL_NODE_LIMIT;
    vfs_node_t *old_node = tty->poll_nodes[index];
    tty->poll_nodes[index] = new_node;
    if (old_node)
        vfs_iput(old_node);
}

void tty_notify_input_ready(tty_t *tty) {
    if (!tty)
        return;

    wait_queue_wake_all(&tty->input_wait, EPOLLIN | EPOLLRDNORM, EOK);
    for (size_t i = 0; i < tty->poll_node_count; i++)
        vfs_poll_notify_inode(tty->poll_nodes[i], EPOLLIN | EPOLLRDNORM);
}

void tty_register_session(tty_t *tty) {
    if (!tty || !llist_empty(&tty->node))
        return;

    llist_append(&tty_session_list, &tty->node);
}

tty_t *tty_lookup_session_by_sid(uint64_t sid) {
    tty_t *pos = NULL;
    tty_t *tmp = NULL;

    if (!sid)
        return NULL;

    llist_for_each(pos, tmp, &tty_session_list, node) {
        if (pos->at_session_id == sid)
            return pos;
    }

    return NULL;
}

void tty_session_attach_current(tty_t *tty) {
    if (!tty || !current_task)
        return;

    tty->at_session_id = current_task->sid;
    tty->at_process_group_id = current_task->pgid;
}

void tty_session_detach_current(tty_t *tty) {
    if (!tty || !current_task)
        return;
    if (tty->at_session_id != current_task->sid)
        return;

    tty->at_session_id = 0;
    tty->at_process_group_id = 0;
}

static void tty_echo_bytes(tty_t *tty, const char *buf, size_t len) {
    if (!tty || !buf || len == 0 || !(tty->termios.c_lflag & ECHO))
        return;

    tty->ops.write(tty, buf, len);
}

static void tty_echo_erase(tty_t *tty) {
    static const char erase_seq[] = "\b \b";

    if (!tty || !(tty->termios.c_lflag & ECHO))
        return;

    if (tty->termios.c_lflag & ECHOE)
        tty->ops.write(tty, erase_seq, sizeof(erase_seq) - 1);
}

static bool tty_is_canon_line_end(tty_t *tty, char c) {
    if (!tty)
        return false;

    if (c == '\n')
        return true;
    if (tty->termios.c_cc[VEOL] && c == tty->termios.c_cc[VEOL])
        return true;
    if (tty->termios.c_cc[VEOL2] && c == tty->termios.c_cc[VEOL2])
        return true;

    return false;
}

static bool tty_input_commit_canon_locked(tty_t *tty) {
    uint16_t old_count;

    if (!tty)
        return false;

    old_count = tty->input_count;
    for (uint16_t i = 0; i < tty->canon_count; i++)
        tty_input_enqueue_byte(tty, tty->canon_buf[i]);
    tty->canon_count = 0;
    return old_count == 0 && tty->input_count > 0;
}

static char tty_shifted_digit(uint16_t code) {
    switch (code) {
    case KEY_1:
        return '!';
    case KEY_2:
        return '@';
    case KEY_3:
        return '#';
    case KEY_4:
        return '$';
    case KEY_5:
        return '%';
    case KEY_6:
        return '^';
    case KEY_7:
        return '&';
    case KEY_8:
        return '*';
    case KEY_9:
        return '(';
    case KEY_0:
        return ')';
    default:
        return 0;
    }
}

typedef struct tty_keymap_entry {
    uint16_t code;
    char normal;
    char shifted;
} tty_keymap_entry_t;

static const tty_keymap_entry_t tty_keymap[] = {
    {KEY_1, '1', '!'},   {KEY_2, '2', '@'},   {KEY_3, '3', '#'},
    {KEY_4, '4', '$'},   {KEY_5, '5', '%'},   {KEY_6, '6', '^'},
    {KEY_7, '7', '&'},   {KEY_8, '8', '*'},   {KEY_9, '9', '('},
    {KEY_0, '0', ')'},   {KEY_A, 'a', 'A'},   {KEY_B, 'b', 'B'},
    {KEY_C, 'c', 'C'},   {KEY_D, 'd', 'D'},   {KEY_E, 'e', 'E'},
    {KEY_F, 'f', 'F'},   {KEY_G, 'g', 'G'},   {KEY_H, 'h', 'H'},
    {KEY_I, 'i', 'I'},   {KEY_J, 'j', 'J'},   {KEY_K, 'k', 'K'},
    {KEY_L, 'l', 'L'},   {KEY_M, 'm', 'M'},   {KEY_N, 'n', 'N'},
    {KEY_O, 'o', 'O'},   {KEY_P, 'p', 'P'},   {KEY_Q, 'q', 'Q'},
    {KEY_R, 'r', 'R'},   {KEY_S, 's', 'S'},   {KEY_T, 't', 'T'},
    {KEY_U, 'u', 'U'},   {KEY_V, 'v', 'V'},   {KEY_W, 'w', 'W'},
    {KEY_X, 'x', 'X'},   {KEY_Y, 'y', 'Y'},   {KEY_Z, 'z', 'Z'},
    {KEY_KP0, '0', '0'}, {KEY_KP1, '1', '1'}, {KEY_KP2, '2', '2'},
    {KEY_KP3, '3', '3'}, {KEY_KP4, '4', '4'}, {KEY_KP5, '5', '5'},
    {KEY_KP6, '6', '6'}, {KEY_KP7, '7', '7'}, {KEY_KP8, '8', '8'},
    {KEY_KP9, '9', '9'},
};

static bool tty_lookup_key_char(uint16_t code, bool shift, bool caps,
                                char *ch) {
    if (!ch)
        return false;

    for (size_t i = 0; i < sizeof(tty_keymap) / sizeof(tty_keymap[0]); i++) {
        if (tty_keymap[i].code != code)
            continue;

        *ch = shift ? tty_keymap[i].shifted : tty_keymap[i].normal;
        if (tty_keymap[i].normal >= 'a' && tty_keymap[i].normal <= 'z' &&
            (shift ^ caps))
            *ch = tty_keymap[i].shifted;
        else if (tty_keymap[i].normal >= 'a' && tty_keymap[i].normal <= 'z' &&
                 !(shift ^ caps))
            *ch = tty_keymap[i].normal;
        return true;
    }

    return false;
}

static bool tty_translate_key(tty_t *tty, uint16_t code, char *out,
                              size_t *out_len) {
    bool shift;
    bool ctrl;
    bool caps;
    char ch = 0;
    const char *seq = NULL;
    size_t len = 0;

    if (!tty || !out || !out_len)
        return false;

    shift = tty->key_shift;
    ctrl = tty->key_ctrl;
    caps = tty->key_capslock;

    switch (code) {
    case KEY_ENTER:
    case KEY_KPENTER:
        ch = '\n';
        break;
    case KEY_ESC:
        ch = 27;
        break;
    case KEY_BACKSPACE:
        ch = 127;
        break;
    case KEY_TAB:
        ch = '\t';
        break;
    case KEY_SPACE:
        ch = ' ';
        break;
    case KEY_MINUS:
        ch = shift ? '_' : '-';
        break;
    case KEY_EQUAL:
    case KEY_KPEQUAL:
        ch = shift ? '+' : '=';
        break;
    case KEY_LEFTBRACE:
        ch = shift ? '{' : '[';
        break;
    case KEY_RIGHTBRACE:
        ch = shift ? '}' : ']';
        break;
    case KEY_BACKSLASH:
        ch = shift ? '|' : '\\';
        break;
    case KEY_SEMICOLON:
        ch = shift ? ':' : ';';
        break;
    case KEY_APOSTROPHE:
        ch = shift ? '"' : '\'';
        break;
    case KEY_GRAVE:
        ch = shift ? '~' : '`';
        break;
    case KEY_COMMA:
        ch = shift ? '<' : ',';
        break;
    case KEY_DOT:
        ch = shift ? '>' : '.';
        break;
    case KEY_SLASH:
    case KEY_KPSLASH:
        ch = shift ? '?' : '/';
        break;
    case KEY_KPASTERISK:
        ch = '*';
        break;
    case KEY_KPMINUS:
        ch = '-';
        break;
    case KEY_KPPLUS:
        ch = '+';
        break;
    case KEY_KPDOT:
        ch = '.';
        break;
    case KEY_UP:
        seq = "\033[A";
        break;
    case KEY_DOWN:
        seq = "\033[B";
        break;
    case KEY_RIGHT:
        seq = "\033[C";
        break;
    case KEY_LEFT:
        seq = "\033[D";
        break;
    case KEY_HOME:
        seq = "\033[H";
        break;
    case KEY_END:
        seq = "\033[F";
        break;
    case KEY_PAGEUP:
        seq = "\033[5~";
        break;
    case KEY_PAGEDOWN:
        seq = "\033[6~";
        break;
    case KEY_INSERT:
        seq = "\033[2~";
        break;
    case KEY_DELETE:
        seq = "\033[3~";
        break;
    default:
        break;
    }

    if (!ch && !seq)
        (void)tty_lookup_key_char(code, shift, caps, &ch);

    if (ctrl && ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 1);
    else if (ctrl && ch >= 'A' && ch <= 'Z')
        ch = (char)(ch - 'A' + 1);

    if (seq) {
        len = strlen(seq);
        memcpy(out, seq, len);
        *out_len = len;
        return true;
    }

    if (!ch)
        return false;

    out[0] = ch;
    *out_len = 1;
    return true;
}

static void tty_receive_bytes(tty_t *tty, const char *buf, size_t len) {
    bool canonical;
    char eofc;
    bool notify = false;

    if (!tty || !buf || len == 0)
        return;

    canonical = (tty->termios.c_lflag & ICANON) != 0;
    eofc = tty->termios.c_cc[VEOF];

    spin_lock(&tty->input_lock);

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];

        if ((tty->termios.c_iflag & IGNCR) && c == '\r')
            continue;
        if ((tty->termios.c_iflag & ICRNL) && c == '\r')
            c = '\n';
        else if ((tty->termios.c_iflag & INLCR) && c == '\n')
            c = '\r';

        if (tty->termios.c_lflag & ISIG) {
            uint64_t pgid = tty->at_process_group_id;
            if (pgid) {
                if (c == tty->termios.c_cc[VINTR]) {
                    send_process_group_signal(pgid, SIGINT);
                    continue;
                }
                if (c == tty->termios.c_cc[VQUIT]) {
                    send_process_group_signal(pgid, SIGQUIT);
                    continue;
                }
                if (c == tty->termios.c_cc[VSUSP]) {
                    send_process_group_signal(pgid, SIGTSTP);
                    continue;
                }
            }
        }

        if (!canonical) {
            bool was_empty = tty->input_count == 0;
            tty_input_enqueue_byte(tty, c);
            if (was_empty)
                notify = true;
            tty_echo_bytes(tty, &c, 1);
            continue;
        }

        if (c == tty->termios.c_cc[VERASE] || c == 127) {
            if (tty->canon_count > 0) {
                tty->canon_count--;
                tty_echo_erase(tty);
            }
            continue;
        }

        if (c == tty->termios.c_cc[VKILL]) {
            while (tty->canon_count > 0) {
                tty->canon_count--;
                tty_echo_erase(tty);
            }
            if (tty->termios.c_lflag & ECHOK)
                tty_echo_bytes(tty, "\n", 1);
            continue;
        }

        if (c == eofc) {
            notify |= tty_input_commit_canon_locked(tty);
            continue;
        }

        if (tty->canon_count < TTY_INPUT_BUF_SIZE)
            tty->canon_buf[tty->canon_count++] = c;
        tty_echo_bytes(tty, &c, 1);

        if (tty_is_canon_line_end(tty, c)) {
            notify |= tty_input_commit_canon_locked(tty);
        }
    }

    spin_unlock(&tty->input_lock);

    if (notify)
        tty_notify_input_ready(tty);
}

static bool tty_input_device_is_keyboard(dev_input_event_t *event) {
    if (!event)
        return false;

    return tty_bitmap_test(event->evbit, EV_KEY) &&
           (tty_bitmap_test(event->keybit, KEY_A) ||
            tty_bitmap_test(event->keybit, KEY_ENTER) ||
            tty_bitmap_test(event->keybit, KEY_SPACE));
}

static bool tty_input_has_data(tty_t *tty) {
    bool ready;

    if (!tty)
        return false;

    spin_lock(&tty->input_lock);
    ready = tty->input_count > 0;
    spin_unlock(&tty->input_lock);
    return ready;
}

static int tty_input_wait(tty_t *tty) {
    wait_queue_entry_t wait;
    int reason;

    if (!tty || !current_task)
        return -EINVAL;

    task_prepare_block(current_task);
    wait_queue_entry_init(&wait, current_task, EPOLLIN | EPOLLRDNORM, NULL,
                          NULL);
    wait_queue_add(&tty->input_wait, &wait);

    if (tty_input_has_data(tty)) {
        wait_queue_remove(&tty->input_wait, &wait);
        task_cancel_block_prepare(current_task);
        return EOK;
    }

    if (task_signal_has_deliverable(current_task)) {
        wait_queue_remove(&tty->input_wait, &wait);
        task_cancel_block_prepare(current_task);
        return -EINTR;
    }

    reason = task_block(current_task, TASK_BLOCKING, -1, "tty_input_read");
    wait_queue_remove(&tty->input_wait, &wait);
    task_cancel_block_prepare(current_task);

    if (reason < 0)
        return reason;
    if (reason != EOK && task_signal_has_deliverable(current_task))
        return -EINTR;
    return EOK;
}

ssize_t tty_input_read(tty_t *tty, char *buf, size_t count, fd_t *fd) {
    size_t read = 0;
    int vmin;

    if (!tty || !buf || count == 0)
        return 0;

    vmin = tty->termios.c_cc[VMIN];
    if (vmin <= 0)
        vmin = 1;

    while (read < count) {
        arch_enable_interrupt();

        if (task_signal_has_deliverable(current_task)) {
            arch_disable_interrupt();
            return read ? read : (size_t)-EINTR;
        }

        spin_lock(&tty->input_lock);
        bool got_byte = tty_input_dequeue_byte(tty, &buf[read]);
        spin_unlock(&tty->input_lock);

        if (got_byte) {
            read++;
            if (!(tty->termios.c_lflag & ICANON) && read >= (size_t)vmin)
                break;
            if ((tty->termios.c_lflag & ICANON) &&
                tty_is_canon_line_end(tty, buf[read - 1]))
                break;
            continue;
        }

        if (read > 0)
            break;

        if (fd && (fd_get_flags(fd) & O_NONBLOCK)) {
            arch_disable_interrupt();
            return -EWOULDBLOCK;
        }

        arch_disable_interrupt();
        int reason = tty_input_wait(tty);
        if (reason < 0)
            return reason;
    }

    arch_disable_interrupt();
    return read;
}

int tty_input_poll(tty_t *tty, int events) {
    ssize_t revents = 0;

    if (!tty)
        return 0;

    spin_lock(&tty->input_lock);
    bool input_ready = tty->input_count > 0;
    spin_unlock(&tty->input_lock);

    if ((events & (EPOLLIN | EPOLLRDNORM)) && input_ready)
        revents |= EPOLLIN | EPOLLRDNORM;
    if (events & (EPOLLOUT | EPOLLWRNORM))
        revents |= EPOLLOUT | EPOLLWRNORM;

    return revents;
}

int tty_input_available(tty_t *tty) {
    if (!tty)
        return 0;

    spin_lock(&tty->input_lock);
    int available = tty->input_count;
    spin_unlock(&tty->input_lock);

    return available;
}

void tty_input_flush(tty_t *tty) {
    if (!tty)
        return;

    spin_lock(&tty->input_lock);
    tty->input_head = 0;
    tty->input_tail = 0;
    tty->input_count = 0;
    tty->canon_count = 0;
    spin_unlock(&tty->input_lock);
    tty_notify_input_ready(tty);
}

void tty_input_event(dev_input_event_t *event, uint16_t type, uint16_t code,
                     int32_t value) {
    tty_t *tty = tty_vt_active();
    char out[8];
    size_t out_len = 0;

    if (!tty || !tty_input_device_is_keyboard(event) || type != EV_KEY)
        return;

    switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        tty->key_shift = value != 0;
        return;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        tty->key_ctrl = value != 0;
        return;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
        tty->key_alt = value != 0;
        return;
    case KEY_CAPSLOCK:
        if (value == 1)
            tty->key_capslock = !tty->key_capslock;
        return;
    default:
        break;
    }

    if (value == 0)
        return;

    if (tty->key_ctrl && tty->key_alt) {
        unsigned int vtnr = 0;

        if (code >= KEY_F1 && code <= KEY_F10)
            vtnr = (unsigned int)(code - KEY_F1 + 1);
        else if (code == KEY_F11)
            vtnr = 11;
        else if (code == KEY_F12)
            vtnr = 12;

        if (vtnr) {
            (void)tty_vt_activate(vtnr);
            return;
        }
    }

    if (tty->tty_kbmode == K_OFF)
        return;

    if (!tty_translate_key(tty, code, out, &out_len))
        return;

    tty_receive_bytes(tty, out, out_len);
}

tty_device_t *alloc_tty_device(enum tty_device_type type) {
    tty_device_t *device = (tty_device_t *)calloc(1, sizeof(tty_device_t));
    device->type = type;
    llist_init_head(&device->node);
    return device;
}

uint64_t register_tty_device(tty_device_t *device) {
    if (device->private_data == NULL)
        return -EINVAL;
    llist_append(&tty_device_list, &device->node);
    return EOK;
}

uint64_t delete_tty_device(tty_device_t *device) {
    if (device == NULL)
        return -EINVAL;
    free(device->private_data);
    llist_delete(&device->node);
    free(device);
    return EOK;
}

tty_device_t *get_tty_device(const char *name) {
    if (name == NULL)
        return NULL;
    tty_device_t *pos = NULL;
    tty_device_t *n = NULL;
    llist_for_each(pos, n, &tty_device_list, node) {
        if (strcmp(pos->name, name) == 0) {
            return pos;
        }
    }
    return NULL;
}

char *default_console = NULL;

void parse_cmdline_console(const char *cmdline) {
    static char console_name[64];
    char buf[64];

    memset(console_name, 0, sizeof(console_name));

    boot_framebuffer_t *boot_fb = boot_get_framebuffer();
    if (!boot_fb) {
        strncpy(console_name, "ttyS0", sizeof(console_name));
        goto next;
    }

    if (!cmdline || !*cmdline) {
        strcpy(console_name, DEFAULT_TTY);
        goto next;
    }

    const char *key = "console=";
    const char *pos = strstr(cmdline, key);
    if (!pos) {
        strcpy(console_name, DEFAULT_TTY);
        goto next;
    }

    pos += strlen(key);

    size_t i = 0;
    while (*pos && *pos != ' ' && i < sizeof(console_name) - 1) {
        console_name[i++] = *pos++;
    }
    console_name[i] = '\0';

next:
    sprintf(buf, "/dev/%s", console_name);

    default_console = strdup(buf);
}

void tty_init() {
    wait_queue_init(&tty_vt_wait);

    boot_framebuffer_t *framebuffer = boot_get_framebuffer();

    if (framebuffer) {
        tty_device_t *fb_device = alloc_tty_device(TTY_DEVICE_GRAPHI);
        struct tty_graphics_ *graphics = malloc(sizeof(struct tty_graphics_));

        graphics->address = (void *)framebuffer->address;
        graphics->width = framebuffer->width;
        graphics->height = framebuffer->height;
        graphics->bpp = framebuffer->bpp;
        graphics->pitch = framebuffer->pitch;

        graphics->blue_mask_shift = framebuffer->blue_mask_shift;
        graphics->red_mask_shift = framebuffer->red_mask_shift;
        graphics->green_mask_shift = framebuffer->green_mask_shift;
        graphics->blue_mask_size = framebuffer->blue_mask_size;
        graphics->red_mask_size = framebuffer->red_mask_size;
        graphics->green_mask_size = framebuffer->green_mask_size;

        fb_device->private_data = graphics;

        char name[32];
        sprintf(name, "tty%lu", 0);
        strcpy(fb_device->name, name);
        register_tty_device(fb_device);
    }

    tty_device_t *serial_dev = alloc_tty_device(TTY_DEVICE_SERIAL);
    struct tty_serial_ *serial = malloc(sizeof(struct tty_serial_));
    serial->port = 0;

    serial_dev->private_data = serial;
    strcpy(serial_dev->name, "ttyS0");
    register_tty_device(serial_dev);

    // 解析命令行 console 参数
    const char *cmdline = boot_get_cmdline();
    parse_cmdline_console(cmdline);

    if (!strncmp(default_console, "/dev/ttyS", 9)) {
        tty_init_session_serial();
    } else {
        tty_init_session();
        tty_init_session_serial();
    }
}

extern uint64_t create_session_terminal(tty_t *tty);
extern void create_session_terminal_serial(tty_t *tty);

static int tty_name_to_vtnr(const char *name, unsigned int *ret) {
    unsigned int n = 0;
    const char *p;

    if (!name || strncmp(name, "tty", 3) != 0 || !name[3])
        return -EINVAL;

    for (p = name + 3; *p; p++) {
        if (*p < '0' || *p > '9')
            return -EINVAL;
        n = n * 10 + (unsigned int)(*p - '0');
        if (n > 63)
            return -ERANGE;
    }

    if (ret)
        *ret = n;
    return 0;
}

static void tty_sysfs_update_active_locked(void) {
    char value[16];
    int len;

    if (!tty_active_node)
        return;

    len = snprintf(value, sizeof(value), "tty%u\n", tty_active_vt);
    if (len > 0) {
        sysfs_write_node(tty_active_node, value, (size_t)len, 0);
        sysfs_notify_node(tty_active_node);
    }
}

void tty_sysfs_register(uint64_t dev, const char *name) {
    char device_path[128];
    unsigned int vtnr;
    vfs_node_t *root;

    if (!dev || !name)
        return;

    snprintf(device_path, sizeof(device_path), "/sys/devices/virtual/tty/%s",
             name);
    root = sysfs_regist_dev('c', (int)((dev >> 8) & 0xff), (int)(dev & 0xff),
                            device_path, name, "SUBSYSTEM=tty\n",
                            "/sys/class/tty", "/sys/class/tty", name, NULL);
    if (!root)
        return;

    if (tty_name_to_vtnr(name, &vtnr) == 0) {
        spin_lock(&tty_vt_lock);
        if (vtnr == 0) {
            if (tty_active_node)
                vfs_iput(tty_active_node);
            tty_active_node = sysfs_child_append(root, "active", false);
            tty_sysfs_update_active_locked();
        }
        spin_unlock(&tty_vt_lock);
    }

    vfs_iput(root);
}

int tty_vt_activate(unsigned int vtnr) {
    tty_t *old_tty;
    tty_t *new_tty;
    uint64_t controller = 0;
    int relsig = 0;

    if (vtnr == 0 || vtnr > 63)
        return -EINVAL;

    spin_lock(&tty_vt_lock);
    new_tty = tty_vts[vtnr];
    if (!new_tty) {
        spin_unlock(&tty_vt_lock);
        return -ENXIO;
    }

    new_tty->vt_allocated = true;
    tty_vt_present |= 1ULL << vtnr;
    if (tty_active_vt == vtnr) {
        spin_unlock(&tty_vt_lock);
        return 0;
    }
    if (tty_pending_vt) {
        int ret = tty_pending_vt == vtnr ? 0 : -EBUSY;
        spin_unlock(&tty_vt_lock);
        return ret;
    }

    old_tty = tty_vts[tty_active_vt];
    if (old_tty && old_tty->current_vt_mode.mode == VT_PROCESS &&
        old_tty->vt_controller_tgid && old_tty->current_vt_mode.relsig > 0) {
        tty_pending_vt = vtnr;
        controller = old_tty->vt_controller_tgid;
        relsig = old_tty->current_vt_mode.relsig;
        spin_unlock(&tty_vt_lock);

        if (task_kill_thread_group(controller, relsig) > 0)
            return 0;

        spin_lock(&tty_vt_lock);
        if (tty_pending_vt != vtnr) {
            spin_unlock(&tty_vt_lock);
            return 0;
        }
    }

    old_tty = tty_vts[tty_active_vt];
    tty_active_vt = vtnr;
    tty_pending_vt = 0;
    tty_sysfs_update_active_locked();
    spin_unlock(&tty_vt_lock);

    terminal_set_active(old_tty, false);
    terminal_set_active(new_tty, true);
    wait_queue_wake_all(&tty_vt_wait, 0, EOK);

    if (new_tty->current_vt_mode.mode == VT_PROCESS &&
        new_tty->vt_controller_tgid && new_tty->current_vt_mode.acqsig > 0)
        task_kill_thread_group(new_tty->vt_controller_tgid,
                               new_tty->current_vt_mode.acqsig);
    return 0;
}

bool tty_vt_is_active(const tty_t *tty) {
    bool active;

    if (!tty || tty->vtnr == 0)
        return false;
    spin_lock(&tty_vt_lock);
    active = tty_active_vt == tty->vtnr;
    spin_unlock(&tty_vt_lock);
    return active;
}

tty_t *tty_vt_active(void) {
    tty_t *tty;

    spin_lock(&tty_vt_lock);
    tty = tty_vts[tty_active_vt];
    spin_unlock(&tty_vt_lock);
    return tty ? tty : kernel_session;
}

int tty_vt_waitactive(unsigned int vtnr) {
    if (vtnr == 0 || vtnr > 63 || !tty_vts[vtnr])
        return -ENXIO;

    for (;;) {
        wait_queue_entry_t wait;

        spin_lock(&tty_vt_lock);
        bool active = tty_active_vt == vtnr;
        spin_unlock(&tty_vt_lock);
        if (active)
            return 0;
        if (task_signal_has_deliverable(current_task))
            return -EINTR;

        task_prepare_block(current_task);
        wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
        wait_queue_add(&tty_vt_wait, &wait);

        spin_lock(&tty_vt_lock);
        active = tty_active_vt == vtnr;
        spin_unlock(&tty_vt_lock);
        if (active) {
            wait_queue_remove(&tty_vt_wait, &wait);
            task_cancel_block_prepare(current_task);
            return 0;
        }

        int reason =
            task_block(current_task, TASK_BLOCKING, -1, "tty_vt_waitactive");
        wait_queue_remove(&tty_vt_wait, &wait);
        task_cancel_block_prepare(current_task);
        if (reason < 0)
            return reason;
        if (task_signal_has_deliverable(current_task))
            return -EINTR;
    }
}

int tty_vt_openqry(void) {
    int ret = -1;

    spin_lock(&tty_vt_lock);
    for (unsigned int i = 1; i < 64; i++) {
        if (tty_vts[i] && !tty_vts[i]->vt_allocated) {
            ret = (int)i;
            break;
        }
    }
    spin_unlock(&tty_vt_lock);
    return ret;
}

int tty_vt_reldisp(tty_t *tty, unsigned int action) {
    tty_t *old_tty;
    tty_t *new_tty;
    unsigned int target;

    if (!tty || tty->vtnr == 0)
        return -EINVAL;
    if (action == VT_ACKACQ)
        return 0;
    if (action > 1)
        return -EINVAL;

    spin_lock(&tty_vt_lock);
    if (tty_active_vt != tty->vtnr || !tty_pending_vt) {
        spin_unlock(&tty_vt_lock);
        return -EINVAL;
    }
    if (action == 0) {
        tty_pending_vt = 0;
        spin_unlock(&tty_vt_lock);
        return 0;
    }

    target = tty_pending_vt;
    old_tty = tty_vts[tty_active_vt];
    new_tty = tty_vts[target];
    tty_active_vt = target;
    tty_pending_vt = 0;
    tty_sysfs_update_active_locked();
    spin_unlock(&tty_vt_lock);

    terminal_set_active(old_tty, false);
    terminal_set_active(new_tty, true);
    wait_queue_wake_all(&tty_vt_wait, 0, EOK);
    if (new_tty && new_tty->current_vt_mode.mode == VT_PROCESS &&
        new_tty->vt_controller_tgid && new_tty->current_vt_mode.acqsig > 0)
        task_kill_thread_group(new_tty->vt_controller_tgid,
                               new_tty->current_vt_mode.acqsig);
    return 0;
}

int tty_vt_disallocate(unsigned int vtnr) {
    if (vtnr > 63)
        return -EINVAL;

    spin_lock(&tty_vt_lock);
    if (vtnr && vtnr == tty_active_vt) {
        spin_unlock(&tty_vt_lock);
        return -EBUSY;
    }
    for (unsigned int i = vtnr ? vtnr : 1; i < 64; i++) {
        tty_t *tty = tty_vts[i];
        if (!tty || i == tty_active_vt)
            goto next;
        tty->vt_allocated = false;
        tty->current_vt_mode.mode = VT_AUTO;
        tty->vt_controller_tgid = 0;
        tty_vt_present &= ~(1ULL << i);
    next:
        if (vtnr)
            break;
    }
    spin_unlock(&tty_vt_lock);
    return 0;
}

int tty_vt_get_state(struct vt_state *state) {
    if (!state)
        return -EINVAL;

    spin_lock(&tty_vt_lock);
    state->v_active = (unsigned short)tty_active_vt;
    state->v_signal = 0;
    state->v_state = (unsigned short)(tty_vt_present & 0xffff);
    spin_unlock(&tty_vt_lock);
    return 0;
}

int tty_ioctl(void *dev, int cmd, void *args) {
    tty_t *tty = dev == tty0_proxy ? tty_vt_active() : dev;
    if (!tty)
        return -ENXIO;
    return tty->ops.ioctl(tty, cmd, (uint64_t)args);
}

int tty_poll(void *dev, int events, fd_t *fd) {
    tty_t *tty = dev == tty0_proxy ? tty_vt_active() : dev;
    if (!tty)
        return -ENXIO;
    (void)fd;
    return tty->ops.poll(tty, events);
}

int tty_read(void *dev, void *buf, uint64_t offset, size_t size, fd_t *fd) {
    tty_t *tty = dev == tty0_proxy ? tty_vt_active() : dev;
    if (!tty)
        return -ENXIO;
    (void)offset;
    return tty->ops.read(tty, buf, size, fd);
}

int tty_write(void *dev, const void *buf, uint64_t offset, size_t size,
              fd_t *fd) {
    tty_t *tty = dev == tty0_proxy ? tty_vt_active() : dev;
    if (!tty)
        return -ENXIO;
    (void)offset;
    (void)fd;
    return tty->ops.write(tty, buf, size);
}

static ssize_t tty_vt_open(void *dev, void *arg) {
    tty_t *tty = dev;
    (void)arg;

    if (!tty)
        return -ENXIO;
    if (tty->vtnr) {
        spin_lock(&tty_vt_lock);
        tty->vt_open_count++;
        tty->vt_allocated = true;
        tty_vt_present |= 1ULL << tty->vtnr;
        spin_unlock(&tty_vt_lock);
    }
    return 0;
}

static ssize_t tty_vt_close(void *dev, void *arg) {
    tty_t *tty = dev;
    (void)arg;

    if (!tty || !tty->vtnr)
        return 0;

    spin_lock(&tty_vt_lock);
    if (tty->vt_open_count > 0)
        tty->vt_open_count--;
    if (tty->vt_open_count == 0 && tty->vtnr != tty_active_vt &&
        tty->at_session_id == 0) {
        tty->vt_allocated = false;
        tty_vt_present &= ~(1ULL << tty->vtnr);
    }
    spin_unlock(&tty_vt_lock);
    return 0;
}

void tty_init_session() {
    const char *tty_name = "tty0";
    tty_device_t *device = get_tty_device(tty_name);
    if (!device) {
        printk("tty_init_session: no device '%s', fallback to last tty\n",
               tty_name);
        device = container_of(tty_device_list.prev, tty_device_t, node);
    }

    tty0_proxy = calloc(1, sizeof(*tty0_proxy));
    uint64_t tty0_dev = device_install(DEV_CHAR, DEV_TTY, tty0_proxy, tty_name,
                                       0, tty_vt_open, tty_vt_close, tty_ioctl,
                                       tty_poll, tty_read, tty_write, NULL);
    tty_sysfs_register(tty0_dev, tty_name);

    for (unsigned int i = 1; i < 64; i++) {
        char name[16];
        tty_t *tty = calloc(1, sizeof(*tty));
        if (!tty)
            break;

        tty->device = device;
        tty->vtnr = i;
        tty->vt_allocated = i == 1;
        spin_init(&tty->input_lock);
        wait_queue_init(&tty->input_wait);
        llist_init_head(&tty->node);
        if ((int64_t)create_session_terminal(tty) < 0) {
            free(tty);
            break;
        }
        terminal_set_active(tty, i == 1);
        tty_register_session(tty);
        tty_vts[i] = tty;
        if (i == 1)
            tty_vt_present |= 1ULL << i;

        snprintf(name, sizeof(name), "tty%u", i);
        uint64_t dev = device_install(DEV_CHAR, DEV_TTY, tty, name, 0,
                                      tty_vt_open, tty_vt_close, tty_ioctl,
                                      tty_poll, tty_read, tty_write, NULL);
        tty_sysfs_register(dev, name);
    }

    kernel_session = tty_vts[1];
    terminal_set_active(kernel_session, true);
}

void tty_init_session_serial() {
    const char *tty_name = "ttyS0";
    tty_device_t *device = get_tty_device(tty_name);
    if (!device) {
        printk("tty_init_serial: device not found: %s\n", tty_name);
        return;
    }

    tty_t *tty = calloc(1, sizeof(tty_t));
    tty->device = device;
    spin_init(&tty->input_lock);
    wait_queue_init(&tty->input_wait);
    llist_init_head(&tty->node);
    create_session_terminal_serial(tty);
    tty_register_session(tty);
    uint64_t dev =
        device_install(DEV_CHAR, DEV_TTY, tty, tty_name, 0, NULL, NULL,
                       tty_ioctl, tty_poll, tty_read, tty_write, NULL);
    tty_sysfs_register(dev, tty_name);

    if (!kernel_session)
        kernel_session = tty;
}
