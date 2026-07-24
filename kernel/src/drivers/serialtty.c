#include <drivers/tty.h>
#include <dev/device.h>
#include <task/signal.h>
#include <drivers/serialtty.h>
#include <drivers/serial.h>
#include <mm/mm.h>
#include <libs/keys.h>
#include <task/task.h>
#include <fs/vfs/fcntl.h>

void terminal_flush_serial(tty_t *session) {
    // flanterm_flush(session->terminal);
    return;
}

extern void send_process_group_signal(int pgid, int sig);

static void terminal_serial_notify_readable(void *opaque) {
    tty_notify_input_ready((tty_t *)opaque);
}

ssize_t terminal_read_serial(tty_t *device, char *buf, size_t count, fd_t *fd) {
    size_t read = 0;
    bool canonical = (device->termios.c_lflag & ICANON) != 0;
    char eofc = device->termios.c_cc[VEOF];
    int vmin = device->termios.c_cc[VMIN];

    (void)fd;

    while (read < count) {
        arch_enable_interrupt();

        if (task_signal_has_deliverable(current_task)) {
            arch_disable_interrupt();
            return read ? read : (size_t)-EINTR;
        }

        char c;
        if (serial_read(&c)) {
            if (device->termios.c_lflag & ISIG) {
                uint64_t pgid = device->at_process_group_id;
                if (pgid) {
                    if (c == device->termios.c_cc[VINTR]) {
                        send_process_group_signal(pgid, SIGINT);
                        continue;
                    }
                    if (c == device->termios.c_cc[VQUIT]) {
                        send_process_group_signal(pgid, SIGQUIT);
                        continue;
                    }
                    if (c == device->termios.c_cc[VSUSP]) {
                        send_process_group_signal(pgid, SIGTSTP);
                        continue;
                    }
                }
            }
            if ((device->termios.c_iflag & ICRNL) && c == '\r')
                c = '\n';
            if (canonical) {
                if (c == eofc) {
                    break;
                }
                buf[read++] = c;
                if (c == '\n')
                    break;
            } else {
                buf[read++] = c;
                if (vmin == 0 || read >= (size_t)vmin)
                    break;
            }
        } else {
            arch_wait_for_interrupt();
        }
    }

    arch_disable_interrupt();

    return read;
}

int terminal_ioctl_serial(tty_t *device, uint32_t cmd, uint64_t arg) {
    switch (cmd) {
    case TIOCGWINSZ: {
        struct winsize ws = {
            .ws_xpixel = 0,
            .ws_ypixel = 0,
            .ws_col = 80,
            .ws_row = 24,
        };
        if (!arg || copy_to_user((void *)arg, &ws, sizeof(ws)))
            return -EFAULT;
        return 0;
    }
    case TIOCSCTTY:
        tty_session_attach_current(device);
        return 0;
    case TIOCGPGRP: {
        int pid = device->at_process_group_id;
        if (!arg || copy_to_user((void *)arg, &pid, sizeof(pid)))
            return -EFAULT;
        return 0;
    }
    case TIOCSPGRP: {
        int pid = 0;
        if (!arg || copy_from_user(&pid, (void *)arg, sizeof(pid)))
            return -EFAULT;
        tty_session_attach_current(device);
        device->at_process_group_id = pid;
        return 0;
    }
    case TIOCGSID: {
        int sid = (int)device->at_session_id;
        if (!sid)
            return -ENOTTY;
        if (!arg || copy_to_user((void *)arg, &sid, sizeof(sid)))
            return -EFAULT;
        return 0;
    }
    case TCGETS:
        if (!arg ||
            copy_to_user((void *)arg, &device->termios, sizeof(termios))) {
            return -EFAULT;
        }
        return 0;
    case TCGETS2: {
        struct termios2 t2 = {0};
        memcpy(&t2.c_iflag, &device->termios.c_iflag, sizeof(uint32_t));
        memcpy(&t2.c_oflag, &device->termios.c_oflag, sizeof(uint32_t));
        memcpy(&t2.c_cflag, &device->termios.c_cflag, sizeof(uint32_t));
        memcpy(&t2.c_lflag, &device->termios.c_lflag, sizeof(uint32_t));
        t2.c_line = device->termios.c_line;
        memcpy(t2.c_cc, device->termios.c_cc, sizeof(t2.c_cc));
        t2.c_ispeed = 0; // Not supported
        t2.c_ospeed = 0; // Not supported
        if (!arg || copy_to_user((void *)arg, &t2, sizeof(struct termios2)))
            return -EFAULT;
        return 0;
    }
    case TCSETS:
        if (!arg ||
            copy_from_user(&device->termios, (void *)arg, sizeof(termios))) {
            return -EFAULT;
        }
        return 0;
    case TCSETS2: {
        struct termios2 t2_set;
        if (!arg ||
            copy_from_user(&t2_set, (void *)arg, sizeof(struct termios2)))
            return -EFAULT;
        memcpy(&device->termios.c_iflag, &t2_set.c_iflag, sizeof(uint32_t));
        memcpy(&device->termios.c_oflag, &t2_set.c_oflag, sizeof(uint32_t));
        memcpy(&device->termios.c_cflag, &t2_set.c_cflag, sizeof(uint32_t));
        memcpy(&device->termios.c_lflag, &t2_set.c_lflag, sizeof(uint32_t));
        device->termios.c_line = t2_set.c_line;
        memcpy(device->termios.c_cc, t2_set.c_cc, sizeof(t2_set.c_cc));
        // Ignore ispeed and ospeed as they are not supported
        return 0;
    }
    case TCSETSW:
        if (!arg ||
            copy_from_user(&device->termios, (void *)arg, sizeof(termios))) {
            return -EFAULT;
        }
        return 0;
    case TIOCSWINSZ:
        return 0;
    case KDGETMODE: {
        int mode = device->tty_mode;
        if (!arg || copy_to_user((void *)arg, &mode, sizeof(mode)))
            return -EFAULT;
        return 0;
    }
    case KDSETMODE:
        if (arg != KD_TEXT && arg != KD_GRAPHICS)
            return -EINVAL;
        device->tty_mode = arg;
        return 0;
    case KDGKBMODE: {
        int kbmode = device->tty_kbmode;
        if (!arg || copy_to_user((void *)arg, &kbmode, sizeof(kbmode)))
            return -EFAULT;
        return 0;
    }
    case KDSKBMODE:
        device->tty_kbmode = arg;
        return 0;
    case VT_SETMODE:
        if (!arg || copy_from_user(&device->current_vt_mode, (void *)arg,
                                   sizeof(struct vt_mode)))
            return -EFAULT;
        return 0;
    case VT_GETMODE:
        if (!arg || copy_to_user((void *)arg, &device->current_vt_mode,
                                 sizeof(struct vt_mode)))
            return -EFAULT;
        return 0;
    case VT_ACTIVATE:
        return tty_vt_activate((unsigned int)arg);
    case VT_WAITACTIVE:
        return 0;
    case VT_GETSTATE: {
        struct vt_state state = {0};
        int ret = tty_vt_get_state(&state);
        if (ret < 0)
            return ret;
        if (!arg || copy_to_user((void *)arg, &state, sizeof(state)))
            return -EFAULT;
        return 0;
    }
    case VT_OPENQRY: {
        int query = 1;
        if (!arg || copy_to_user((void *)arg, &query, sizeof(query)))
            return -EFAULT;
        return 0;
    }
    case TIOCNOTTY:
        tty_session_detach_current(device);
        return 0;
    case TCSETSF:
        if (!arg ||
            copy_from_user(&device->termios, (void *)arg, sizeof(termios)))
            return -EFAULT;
        return 0;
    case TCFLSH:
        return 0;
    case FIONREAD: {
        int available = serial_can_read() ? 1 : 0;
        if (!arg || copy_to_user((void *)arg, &available, sizeof(available)))
            return -EFAULT;
        return 0;
    }
    case TCSBRK:
    case TCSBRKP:
        return 0;
    case TIOCNXCL:
        return 0;
    default:
        return -ENOTTY;
    }
}

int terminal_poll_serial(tty_t *device, int events) {
    ssize_t revents = 0;

    if ((events & EPOLLIN) && serial_can_read())
        revents |= EPOLLIN;
    if (events & EPOLLOUT)
        revents |= EPOLLOUT;

    return revents;
}

spinlock_t terminal_write_serial_lock = SPIN_INIT;

size_t terminal_write_serial(tty_t *device, const char *buf, size_t count) {
    spin_lock(&terminal_write_serial_lock);
    serial_printk(buf, count);
    spin_unlock(&terminal_write_serial_lock);
    return count;
}

uint64_t create_session_terminal_serial(tty_t *session) {
    if (session->device == NULL)
        return -ENODEV;
    if (session->device->type != TTY_DEVICE_SERIAL)
        return -EINVAL;

    session->terminal = NULL;
    memset(&session->termios, 0, sizeof(termios));
    session->termios.c_iflag = BRKINT | ICRNL | INPCK | ISTRIP | IXON;
    session->termios.c_oflag = OPOST;
    session->termios.c_cflag = CS8 | CREAD | CLOCAL;
    session->termios.c_lflag = ECHO | ICANON | IEXTEN | ISIG;
    session->termios.c_line = 0;
    session->termios.c_cc[VINTR] = 3;     // Ctrl-C
    session->termios.c_cc[VQUIT] = 28;    // Ctrl-Backslash
    session->termios.c_cc[VERASE] = 127;  // DEL
    session->termios.c_cc[VKILL] = 21;    // Ctrl-U
    session->termios.c_cc[VEOF] = 4;      // Ctrl-D
    session->termios.c_cc[VTIME] = 0;     // No timer
    session->termios.c_cc[VMIN] = 1;      // Return each byte
    session->termios.c_cc[VSTART] = 17;   // Ctrl-Q
    session->termios.c_cc[VSTOP] = 19;    // Ctrl-S
    session->termios.c_cc[VSUSP] = 26;    // Ctrl-Z
    session->termios.c_cc[VREPRINT] = 18; // Ctrl-R
    session->termios.c_cc[VDISCARD] = 15; // Ctrl-O
    session->termios.c_cc[VWERASE] = 23;  // Ctrl-W
    session->termios.c_cc[VLNEXT] = 22;   // Ctrl-V
    // Initialize other control characters to 0
    for (int i = 16; i < NCCS; i++) {
        session->termios.c_cc[i] = 0;
    }

    session->tty_mode = KD_TEXT;
    session->tty_kbmode = K_XLATE;
    // session->terminal = fl_context;
    session->ops.flush = terminal_flush_serial;
    session->ops.ioctl = terminal_ioctl_serial;
    session->ops.poll = terminal_poll_serial;
    session->ops.read = terminal_read_serial;
    session->ops.write = terminal_write_serial;
    serial_register_readable_notifier(terminal_serial_notify_readable, session);

    return EOK;
}
