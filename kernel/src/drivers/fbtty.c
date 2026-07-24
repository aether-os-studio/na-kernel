#include <drivers/tty.h>
#include <mm/mm.h>
#include <task/signal.h>
#include <drivers/fbtty.h>
#include <drivers/serial.h>
#include <libs/keys.h>
#include <task/task.h>
#include <libs/termios.h>
#include <fs/vfs/fcntl.h>
#define FLANTERM_IN_FLANTERM
#include <libs/flanterm/flanterm_private.h>
#include <libs/flanterm/flanterm.h>
#include <libs/flanterm/flanterm_backends/fb_private.h>
#include <libs/flanterm/flanterm_backends/fb.h>

void terminal_flush(tty_t *session) { flanterm_flush(session->terminal); }

extern void send_process_group_signal(int pgid, int sig);

ssize_t terminal_read(tty_t *device, char *buf, size_t count, fd_t *fd) {
    return tty_input_read(device, buf, count, fd);
}

int terminal_ioctl(tty_t *device, uint32_t cmd, uint64_t arg) {
    struct flanterm_context *ft_ctx = device->terminal;
    struct flanterm_fb_context *fb_ctx = device->terminal;
    switch (cmd) {
    case TIOCGWINSZ: {
        struct winsize ws = {
            .ws_xpixel = fb_ctx->width,
            .ws_ypixel = fb_ctx->height,
            .ws_col = ft_ctx->cols,
            .ws_row = ft_ctx->rows,
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
        tty_input_flush(device);
        return 0;
    case TCFLSH:
        tty_input_flush(device);
        return 0;
    case FIONREAD: {
        int available = tty_input_available(device);
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

int terminal_poll(tty_t *device, int events) {
    return tty_input_poll(device, events);
}

spinlock_t terminal_write_lock = SPIN_INIT;

size_t terminal_write(tty_t *device, const char *buf, size_t count) {
    spin_lock(&terminal_write_lock);
#if SERIAL_DEBUG
    serial_printk((char *)buf, count);
#endif
    /* KD_TEXT/KD_GRAPHICS controls whether the kernel console owns the
     * framebuffer. VT_AUTO/VT_PROCESS instead controls who arbitrates VT
     * switching and must not be used as a rendering-mode test. */
    if (device->tty_mode == KD_TEXT && device->terminal) {
        flanterm_write(device->terminal, buf, count);
    }
    spin_unlock(&terminal_write_lock);
    return count;
}

uint64_t create_session_terminal(tty_t *session) {
    if (session->device == NULL)
        return -ENODEV;
    if (session->device->type != TTY_DEVICE_GRAPHI)
        return -EINVAL;
    struct tty_graphics_ *framebuffer = session->device->private_data;
    struct flanterm_context *fl_context =
        framebuffer->address
            ? flanterm_fb_init(
                  NULL, NULL, (void *)framebuffer->address, framebuffer->width,
                  framebuffer->height, framebuffer->pitch,
                  framebuffer->red_mask_size, framebuffer->red_mask_shift,
                  framebuffer->green_mask_size, framebuffer->green_mask_shift,
                  framebuffer->blue_mask_size, framebuffer->blue_mask_shift,
                  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0,
                  0, FLANTERM_FB_ROTATE_0)
            : NULL;
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
    session->terminal = fl_context;
    session->ops.flush = terminal_flush;
    session->ops.ioctl = terminal_ioctl;
    session->ops.poll = terminal_poll;
    session->ops.read = terminal_read;
    session->ops.write = terminal_write;
    return EOK;
}
