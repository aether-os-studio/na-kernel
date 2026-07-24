#include <drivers/logger.h>
#include <drivers/tty.h>
#include <dev/device.h>
#include <arch/arch.h>
#include <mm/mm.h>
#include <boot/boot.h>

// util-linux dmesg defaults to /dev/kmsg. Keep records in Linux kmsg order.
#define KMSG_TEXT_BUFFER_SIZE (256 * 1024)
#define KMSG_RECORD_CAPACITY 1024
#define KMSG_PRINTK_BUFFER_SIZE 4096
#define KMSG_MAX_RECORD_TEXT (KMSG_PRINTK_BUFFER_SIZE - 1)

#define SYSLOG_ACTION_CLOSE 0
#define SYSLOG_ACTION_OPEN 1
#define SYSLOG_ACTION_READ 2
#define SYSLOG_ACTION_READ_ALL 3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR 5
#define SYSLOG_ACTION_CONSOLE_OFF 6
#define SYSLOG_ACTION_CONSOLE_ON 7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD 9
#define SYSLOG_ACTION_SIZE_BUFFER 10

#define PAD_ZERO 1 // 0填充
#define LEFT 2     // 靠左对齐
#define RIGHT 4    // 靠右对齐
#define PLUS 8     // 在正数前面显示加号
#define SPACE 16
#define SPECIAL 32 // 在八进制数前面显示 '0o'，在十六进制数前面显示 '0x' 或 '0X'
#define SMALL 64   // 十进制以上数字显示小写字母
#define SIGN 128   // 显示符号位

char buf[KMSG_PRINTK_BUFFER_SIZE];

typedef struct kmsg_record {
    uint64_t seq;
    uint64_t timestamp_us;
    uint32_t text_off;
    uint32_t text_len;
    uint16_t priority;
} kmsg_record_t;

static char kmsg_text_ring[KMSG_TEXT_BUFFER_SIZE];
static kmsg_record_t kmsg_records[KMSG_RECORD_CAPACITY];
static uint32_t kmsg_record_head = 0;
static uint32_t kmsg_record_count = 0;
static uint32_t kmsg_text_head = 0;
static uint32_t kmsg_text_used = 0;
static uint64_t kmsg_next_seq = 0;
static vfs_node_t *kmsg_poll_node = NULL;
spinlock_t printk_lock = SPIN_INIT;

static inline uint64_t kmsg_oldest_seq_locked(void) {
    if (kmsg_record_count == 0)
        return kmsg_next_seq;
    return kmsg_records[kmsg_record_head].seq;
}

static inline kmsg_record_t *kmsg_record_at_seq_locked(uint64_t seq) {
    uint64_t oldest = kmsg_oldest_seq_locked();

    if (seq < oldest || seq >= kmsg_next_seq)
        return NULL;

    return &kmsg_records[(kmsg_record_head + (uint32_t)(seq - oldest)) %
                         KMSG_RECORD_CAPACITY];
}

static void kmsg_copy_from_ring_locked(uint32_t off, char *dst, size_t len) {
    size_t first;

    if (!dst || len == 0)
        return;

    first = MIN(len, KMSG_TEXT_BUFFER_SIZE - off);
    memcpy(dst, kmsg_text_ring + off, first);
    if (len > first)
        memcpy(dst + first, kmsg_text_ring, len - first);
}

static void kmsg_drop_oldest_locked(void) {
    kmsg_record_t *record;

    if (kmsg_record_count == 0)
        return;

    record = &kmsg_records[kmsg_record_head];
    kmsg_text_head =
        (record->text_off + record->text_len) % KMSG_TEXT_BUFFER_SIZE;
    if (kmsg_text_used >= record->text_len)
        kmsg_text_used -= record->text_len;
    else
        kmsg_text_used = 0;
    kmsg_record_head = (kmsg_record_head + 1) % KMSG_RECORD_CAPACITY;
    kmsg_record_count--;
}

static bool kmsg_append_record_locked(const char *text, size_t len,
                                      uint16_t priority) {
    kmsg_record_t *record;
    uint32_t tail;
    size_t first;

    if (!text || len == 0)
        return false;

    if (len > KMSG_MAX_RECORD_TEXT)
        len = KMSG_MAX_RECORD_TEXT;
    if (len > KMSG_TEXT_BUFFER_SIZE)
        len = KMSG_TEXT_BUFFER_SIZE;

    while (kmsg_record_count >= KMSG_RECORD_CAPACITY ||
           kmsg_text_used + len > KMSG_TEXT_BUFFER_SIZE) {
        kmsg_drop_oldest_locked();
    }

    tail = (kmsg_text_head + kmsg_text_used) % KMSG_TEXT_BUFFER_SIZE;
    first = MIN(len, KMSG_TEXT_BUFFER_SIZE - tail);
    memcpy(kmsg_text_ring + tail, text, first);
    if (len > first)
        memcpy(kmsg_text_ring, text + first, len - first);

    record = &kmsg_records[(kmsg_record_head + kmsg_record_count) %
                           KMSG_RECORD_CAPACITY];
    record->seq = kmsg_next_seq++;
    record->timestamp_us = nano_time() / 1000ULL;
    record->text_off = tail;
    record->text_len = (uint32_t)len;
    record->priority = priority;

    kmsg_text_used += (uint32_t)len;
    kmsg_record_count++;
    return true;
}

static size_t kmsg_trim_record_text(char *text, size_t len) {
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
        len--;
    return len;
}

static ssize_t kmsg_snapshot_all(char **snapshot_out, bool clear_after) {
    char *snapshot;
    size_t len;
    size_t to_copy;

    if (!snapshot_out)
        return -EINVAL;

    *snapshot_out = NULL;

    spin_lock(&printk_lock);
    len = kmsg_text_used;
    spin_unlock(&printk_lock);

    if (len == 0)
        return 0;

    snapshot = malloc(len);
    if (!snapshot)
        return -ENOMEM;

    spin_lock(&printk_lock);
    to_copy = MIN(len, (size_t)kmsg_text_used);
    if (to_copy)
        kmsg_copy_from_ring_locked(kmsg_text_head, snapshot, to_copy);
    if (clear_after) {
        kmsg_record_head = 0;
        kmsg_record_count = 0;
        kmsg_text_head = 0;
        kmsg_text_used = 0;
    }
    spin_unlock(&printk_lock);

    *snapshot_out = snapshot;
    return (ssize_t)to_copy;
}

size_t logger_kmsg_buffer_size(void) { return KMSG_TEXT_BUFFER_SIZE; }

static inline uint64_t kmsg_file_seq_get(fd_t *file, uint64_t fallback) {
    void *private_data;

    if (!file)
        return fallback;
    private_data = __atomic_load_n(&file->private_data, __ATOMIC_ACQUIRE);
    if (!private_data)
        return fallback;
    return (uint64_t)((uintptr_t)private_data - 1U);
}

static inline void kmsg_file_seq_set(fd_t *file, uint64_t seq) {
    if (!file)
        return;
    __atomic_store_n(&file->private_data, (void *)(uintptr_t)(seq + 1U),
                     __ATOMIC_RELEASE);
}

static bool kmsg_file_has_unread_locked(fd_t *file) {
    uint64_t oldest;
    uint64_t seq;

    if (kmsg_record_count == 0)
        return false;

    oldest = kmsg_oldest_seq_locked();
    seq = kmsg_file_seq_get(file, oldest);
    if (seq < oldest)
        seq = oldest;
    return seq < kmsg_next_seq;
}

static void kmsg_notify_readers(void) {
    vfs_node_t *node = __atomic_load_n(&kmsg_poll_node, __ATOMIC_ACQUIRE);

    if (node)
        vfs_poll_notify_inode(node, EPOLLIN | EPOLLRDNORM);
}

void logger_kmsg_bind_node(vfs_node_t *node) {
    if (!node)
        return;

    spin_lock(&printk_lock);
    if (!__atomic_load_n(&kmsg_poll_node, __ATOMIC_RELAXED))
        __atomic_store_n(&kmsg_poll_node, vfs_igrab(node), __ATOMIC_RELEASE);
    spin_unlock(&printk_lock);
}

ssize_t logger_kmsg_poll(fd_t *file, int events) {
    ssize_t revents = 0;

    if (file)
        logger_kmsg_bind_node(file->f_inode);

    spin_lock(&printk_lock);
    if ((events & (EPOLLIN | EPOLLRDNORM)) && kmsg_file_has_unread_locked(file))
        revents |= EPOLLIN | EPOLLRDNORM;
    spin_unlock(&printk_lock);

    if (events & EPOLLOUT)
        revents |= EPOLLOUT | EPOLLWRNORM;

    return revents;
}

ssize_t logger_kmsg_read(fd_t *file, void *buf, size_t len, uint64_t flags) {
    kmsg_record_t record;
    char text[KMSG_MAX_RECORD_TEXT + 1];
    char header[96];
    size_t text_len;
    int header_len;
    size_t total;
    uint64_t seq;
    uint64_t oldest;

    if (!file || !buf)
        return -EINVAL;

    logger_kmsg_bind_node(file->f_inode);

    while (true) {
        spin_lock(&printk_lock);
        if (!kmsg_file_has_unread_locked(file)) {
            spin_unlock(&printk_lock);
            if (flags & O_NONBLOCK)
                return -EWOULDBLOCK;

            int reason = vfs_poll_wait_interruptible(
                file, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLNVAL);
            if (reason < 0)
                return reason;
            continue;
        }

        oldest = kmsg_oldest_seq_locked();
        seq = kmsg_file_seq_get(file, oldest);
        if (seq < oldest)
            seq = oldest;
        record = *kmsg_record_at_seq_locked(seq);
        kmsg_copy_from_ring_locked(record.text_off, text, record.text_len);
        spin_unlock(&printk_lock);
        break;
    }

    text_len = kmsg_trim_record_text(text, record.text_len);
    text[text_len] = '\0';

    header_len =
        snprintf(header, sizeof(header), "%u,%llu,%llu,-;",
                 (unsigned)record.priority, (unsigned long long)record.seq,
                 (unsigned long long)record.timestamp_us);
    if (header_len < 0)
        return -EINVAL;

    total = (size_t)header_len + text_len + 1;
    if (len < total)
        return -EINVAL;
    memcpy(buf, header, (size_t)header_len);
    if (text_len)
        memcpy((char *)buf + header_len, text, text_len);
    ((char *)buf)[header_len + text_len] = '\n';

    kmsg_file_seq_set(file, record.seq + 1);
    return (ssize_t)total;
}

ssize_t logger_kmsg_write(const void *buf, size_t len) {
    char chunk[KMSG_MAX_RECORD_TEXT];
    size_t offset = 0;
    ssize_t written = 0;

    if (!buf)
        return -EINVAL;
    if (len == 0)
        return 0;

    while (offset < len) {
        size_t part = MIN(sizeof(chunk), len - offset);

        memcpy(chunk, (const char *)buf + offset, part);

        spin_lock(&printk_lock);
        kmsg_append_record_locked(chunk, part, 6);
        spin_unlock(&printk_lock);
        kmsg_notify_readers();

        offset += part;
        written += (ssize_t)part;
    }

    return written;
}

static int get_atoi(const char **str) {
    int n;
    for (n = 0; is_digit(**str); (*str)++)
        n = n * 10 + **str - '0';
    return n;
}

static void bputc(char *buf, size_t *pos, size_t max, char c) {
    if (*pos < max)
        buf[(*pos)] = c;
    (*pos)++;
}

#define F_ALTERNATE 0001 // put 0x infront 16, 0 on octals, b on binary
#define F_ZEROPAD 0002   // value should be zero padded
#define F_LEFT 0004      // left justified if set, otherwise right justified
#define F_SPACE 0010     // place a space before positive number
#define F_PLUS 0020      // show +/- on signed numbers, default only for -
#define F_SIGNED 0040    // is an unsigned number?
#define F_SMALL 0100     // use lowercase for hex?

/**
 * Formats an integer number
 *  buf - buffer to print into
 *  len - current position in buffer
 *  maxlen - last valid position in buf
 *  num - number to print
 *  base - it's base
 *  width - how many spaces this should have; padding
 *  flags - above F flags
 */
static void fmt_int(char *buf, size_t *len, size_t maxlen, long long num,
                    int base, int width, int flags) {
    char nbuf[64], sign = 0;
    char altb[8]; // small buf for sign and #
    unsigned long n = num;
    int npad;         // number of pads
    char pchar = ' '; // padding character
    char *digits = "0123456789ABCDEF";
    char *ldigits = "0123456789abcdef";
    int i, j;

    if (base < 2 || base > 16)
        return;
    if (flags & F_SMALL)
        digits = ldigits;
    if (flags & F_LEFT)
        flags &= ~F_ZEROPAD;

    if ((flags & F_SIGNED) && num < 0) {
        n = -num;
        sign = '-';
    } else if (flags & F_PLUS) {
        sign = '+';
    } else if (flags & F_SPACE)
        sign = ' ';

    i = 0;
    do {
        nbuf[i++] = digits[n % base];
        n = n / base;
    } while (n > 0);

    j = 0;
    if (sign)
        altb[j++] = sign;
    if (flags & F_ALTERNATE) {
        if (base == 8 || base == 16) {
            altb[j++] = '0';
            if (base == 16)
                altb[j++] = (flags & F_SMALL) ? 'x' : 'X';
        }
    }
    altb[j] = 0;

    npad = width > i + j ? width - i - j : 0;

    if (width > i + j)
        npad = width - i - j;

    if (npad > 0 && ((flags & F_LEFT) == 0)) {
        if (flags & F_ZEROPAD) {
            for (j = 0; altb[j]; j++)
                bputc(buf, len, maxlen, altb[j]);
            altb[0] = 0;
        }
        while (npad-- > 0)
            bputc(buf, len, maxlen, (flags & F_ZEROPAD) ? '0' : ' ');
    }
    for (j = 0; altb[j]; j++)
        bputc(buf, len, maxlen, altb[j]);

    while (i-- > 0)
        bputc(buf, len, maxlen, nbuf[i]);

    if (npad > 0 && (flags & F_LEFT))
        while (npad-- > 0)
            bputc(buf, len, maxlen, pchar);
}

static void fmt_chr(char *buf, size_t *pos, size_t max, char c, int width,
                    int flags) {
    int npad = 0;
    if (width > 0)
        npad = width - 1;
    if (npad < 0)
        npad = 0;

    if (npad && ((flags & F_LEFT) == 0))
        while (npad-- > 0)
            bputc(buf, pos, max, ' ');

    bputc(buf, pos, max, c);

    if (npad && (flags & F_LEFT))
        while (npad-- > 0)
            bputc(buf, pos, max, ' ');
}

/**
 * strlen()
 */
static size_t slen(char *s) {
    size_t i;
    for (i = 0; *s; i++, s++)
        ;
    return i;
}

static void fmt_str(char *buf, size_t *pos, size_t max, char *s, int width,
                    int precision, int flags) {
    int len = 0;
    int npad = 0;

    if (precision < 0) {
        len = slen(s);
    } else {
        while (s[len] && len < precision)
            len++;
    }

    if (width > 0)
        npad = width - len;
    if (npad < 0)
        npad = 0;

    if (npad && ((flags & F_LEFT) == 0))
        while (npad-- > 0)
            bputc(buf, pos, max, ' ');

    while (len-- > 0)
        bputc(buf, pos, max, *s++);

    if (npad && (flags & F_LEFT))
        while (npad-- > 0)
            bputc(buf, pos, max, ' ');
}

/* Format states */
#define S_DEFAULT 0
#define S_FLAGS 1
#define S_WIDTH 2
#define S_PRECIS 3
#define S_LENGTH 4
#define S_CONV 5

/* Lenght flags */
#define L_CHAR 1
#define L_SHORT 2
#define L_LONG 3
#define L_LLONG 4
#define L_DOUBLE 5

/**
 * Shrinked down, vsnprintf implementation.
 *  This will not handle floating numbers (yet).
 */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t n = 0;
    char c, *s;
    char state = 0;
    long long num;
    int base;
    int flags, width, precision, lflags;

    if (!buf)
        size = 0;

    for (;;) {
        c = *fmt++;
        if (state == S_DEFAULT) {
            if (c == '%') {
                state = S_FLAGS;
                flags = 0;
            } else {
                bputc(buf, &n, size, c);
            }
        } else if (state == S_FLAGS) {
            switch (c) {
            case '#':
                flags |= F_ALTERNATE;
                break;
            case '0':
                flags |= F_ZEROPAD;
                break;
            case '-':
                flags |= F_LEFT;
                break;
            case ' ':
                flags |= F_SPACE;
                break;
            case '+':
                flags |= F_PLUS;
                break;
            case '\'':
            case 'I':
                break; // not yet used
            default:
                fmt--;
                width = 0;
                state = S_WIDTH;
            }
        } else if (state == S_WIDTH) {
            if (c == '*') {
                width = va_arg(ap, int);
                if (width < 0) {
                    width = -width;
                    flags |= F_LEFT;
                }
            } else if (is_digit(c) && c > '0') {
                fmt--;
                width = get_atoi(&fmt);
            } else {
                fmt--;
                precision = -1;
                state = S_PRECIS;
            }
        } else if (state == S_PRECIS) {
            // Parse precision
            if (c == '.') {
                if (is_digit(*fmt))
                    precision = get_atoi(&fmt);
                else if (*fmt == '*') {
                    fmt++;
                    precision = va_arg(ap, int);
                } else {
                    precision = 0;
                }
                if (precision < 0)
                    precision = -1;
            } else
                fmt--;
            lflags = 0;
            state = S_LENGTH;
        } else if (state == S_LENGTH) {
            switch (c) {
            case 'h':
                lflags = lflags == L_CHAR ? L_SHORT : L_CHAR;
                break;
            case 'l':
                lflags = lflags == L_LONG ? L_LLONG : L_LONG;
                break;
            case 'z':
                lflags =
                    sizeof(size_t) > sizeof(unsigned long) ? L_LLONG : L_LONG;
                break;
            case 'L':
                lflags = L_DOUBLE;
                break;
            default:
                fmt--;
                state = S_CONV;
            }
        } else if (state == S_CONV) {
            if (c == 'd' || c == 'i' || c == 'o' || c == 'b' || c == 'u' ||
                c == 'x' || c == 'X') {
                if (lflags == L_LONG)
                    num = va_arg(ap, long);
                else if (lflags & (L_LLONG | L_DOUBLE))
                    num = va_arg(ap, long long);
                else if (c == 'd' || c == 'i')
                    num = va_arg(ap, int);
                else
                    num = (unsigned int)va_arg(ap, int);

                base = 10;
                if (c == 'd' || c == 'i') {
                    flags |= F_SIGNED;
                } else if (c == 'x' || c == 'X') {
                    flags |= c == 'x' ? F_SMALL : 0;
                    base = 16;
                } else if (c == 'o') {
                    base = 8;
                } else if (c == 'b') {
                    base = 2;
                }
                fmt_int(buf, &n, size, num, base, width, flags);
            } else if (c == 'p') {
                num = (long)va_arg(ap, void *);
                base = 16;
                flags |= F_SMALL | F_ALTERNATE;
                fmt_int(buf, &n, size, num, base, width, flags);
            } else if (c == 's') {
                s = va_arg(ap, char *);
                if (!s)
                    s = "(null)";
                fmt_str(buf, &n, size, s, width, precision, flags);
            } else if (c == 'c') {
                c = va_arg(ap, int);
                fmt_chr(buf, &n, size, c, width, flags);
            } else if (c == '%') {
                bputc(buf, &n, size, c);
            } else {
                bputc(buf, &n, size, '%');
                bputc(buf, &n, size, c);
            }
            state = S_DEFAULT;
        }
        if (c == 0)
            break;
    }
    n--;
    if (n < size)
        buf[n] = 0;
    else if (size > 0)
        buf[size - 1] = 0;

    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, SIZE_MAX, fmt, ap);
}

int printk(const char *fmt, ...) {
    spin_lock(&printk_lock);

    va_list args;
    va_start(args, fmt);

    int len = vsnprintf(buf, sizeof(buf), fmt, args);

    va_end(args);

    if (len < 0) {
        spin_unlock(&printk_lock);
        return len;
    }
    if ((size_t)len >= sizeof(buf))
        len = sizeof(buf) - 1;

    if (len > 0)
        kmsg_append_record_locked(buf, (size_t)len, 6);

    device_t *device = device_find(DEV_TTY, 0);
    if (device)
        device_write(device->dev, buf, 0, len, 0);

#if !SERIAL_DEBUG
    serial_printk(buf, len);
#endif

    spin_unlock(&printk_lock);
    if (len > 0)
        kmsg_notify_readers();

    return len;
}

int serial_fprintk(const char *fmt, ...) {
    spin_lock(&printk_lock);

    va_list args;
    va_start(args, fmt);

    int len = vsnprintf(buf, sizeof(buf), fmt, args);

    va_end(args);

    if (len < 0) {
        spin_unlock(&printk_lock);
        return len;
    }
    if ((size_t)len >= sizeof(buf))
        len = sizeof(buf) - 1;

    if (len > 0)
        kmsg_append_record_locked(buf, (size_t)len, 6);

    serial_printk(buf, len);

    spin_unlock(&printk_lock);
    if (len > 0)
        kmsg_notify_readers();

    return len;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    int len = vsprintf(buf, fmt, args);

    va_end(args);

    return len;
}

int snprintf(char *buffer, size_t capacity, const char *fmt, ...) {
    va_list vlist;
    int ret;

    va_start(vlist, fmt);
    ret = vsnprintf(buffer, capacity, fmt, vlist);
    va_end(vlist);

    return ret;
}

uint64_t sys_syslog(int type, const char *buf, size_t len) {
    char *snapshot = NULL;
    ssize_t snap_len;
    size_t to_copy;

    switch (type) {
    case SYSLOG_ACTION_CLOSE:
    case SYSLOG_ACTION_OPEN:
    case SYSLOG_ACTION_CONSOLE_OFF:
    case SYSLOG_ACTION_CONSOLE_ON:
        return 0;
    case SYSLOG_ACTION_CONSOLE_LEVEL:
        return len;
    case SYSLOG_ACTION_SIZE_UNREAD:
        spin_lock(&printk_lock);
        to_copy = kmsg_text_used;
        spin_unlock(&printk_lock);
        return to_copy;
    case SYSLOG_ACTION_SIZE_BUFFER:
        return logger_kmsg_buffer_size();
    case SYSLOG_ACTION_CLEAR:
        spin_lock(&printk_lock);
        kmsg_record_head = 0;
        kmsg_record_count = 0;
        kmsg_text_head = 0;
        kmsg_text_used = 0;
        spin_unlock(&printk_lock);
        return 0;
    case SYSLOG_ACTION_READ:
    case SYSLOG_ACTION_READ_ALL:
    case SYSLOG_ACTION_READ_CLEAR:
        if (len == 0)
            return 0;
        if (!buf)
            return (uint64_t)-EFAULT;

        snap_len = kmsg_snapshot_all(
            &snapshot, type == SYSLOG_ACTION_READ_CLEAR ? true : false);
        if (snap_len < 0)
            return (uint64_t)snap_len;

        to_copy = MIN((size_t)snap_len, len);
        if (to_copy && copy_to_user((void *)buf, snapshot, to_copy)) {
            free(snapshot);
            return (uint64_t)-EFAULT;
        }

        free(snapshot);
        return to_copy;
    default:
        return (uint64_t)-EINVAL;
    }
}
