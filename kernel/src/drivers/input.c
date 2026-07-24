#include <drivers/input.h>
#include <fs/dev.h>
#include <fs/sys.h>
#include <drivers/logger.h>
#include <mm/mm.h>

static int eventn = 0;

spinlock_t inputdev_regist_lock = SPIN_INIT;

extern vfs_node_t *devtmpfs_root;

static void input_sysfs_write_file(const char *path, const char *content) {
    vfs_node_t *node;

    if (!path || !content)
        return;

    node = sysfs_ensure_file(path);
    if (!node)
        return;

    sysfs_write_node(node, content, strlen(content), 0);
    vfs_iput(node);
}

static void input_bitmap_to_string(char *out, size_t out_size,
                                   const uint8_t *bitmap, size_t bitmap_bytes) {
    if (!out || out_size == 0)
        return;

    out[0] = '\0';

    size_t word_bytes = sizeof(unsigned long);
    ssize_t last = (ssize_t)((bitmap_bytes + word_bytes - 1) / word_bytes) - 1;
    while (last > 0) {
        bool empty = true;
        size_t start = (size_t)last * word_bytes;
        for (size_t i = start; i < bitmap_bytes && i < start + word_bytes;
             i++) {
            if (bitmap[i] != 0) {
                empty = false;
                break;
            }
        }
        if (!empty)
            break;
        last--;
    }

    size_t off = 0;
    for (ssize_t word = last; word >= 0; word--) {
        unsigned long value = 0;
        size_t start = (size_t)word * word_bytes;
        for (size_t i = 0; i < word_bytes && start + i < bitmap_bytes; i++) {
            value |= (unsigned long)bitmap[start + i] << (i * 8);
        }

        int written =
            snprintf(out + off, out_size - off, word == last ? "%lx" : " %0*lx",
                     value, (int)(word_bytes * 2), value);
        if (written < 0 || (size_t)written >= out_size - off)
            break;
        off += (size_t)written;
    }

    if (off + 1 < out_size) {
        out[off++] = '\n';
        out[off] = '\0';
    }
}

static void input_sysfs_publish_metadata(dev_input_event_t *input_event,
                                         const char *input_dir_path,
                                         const char *input_dirname,
                                         const char *event_dir_path) {
    if (!input_event || !input_dir_path || !input_dirname)
        return;

    char path[512];
    char content[2048];

    snprintf(path, sizeof(path), "/sys/class/input/%s", input_dirname);
    sysfs_ensure_symlink(path, input_dir_path);

    snprintf(path, sizeof(path), "%s/subsystem", input_dir_path);
    sysfs_ensure_symlink(path, "/sys/class/input");

    snprintf(path, sizeof(path), "%s/name", input_dir_path);
    snprintf(content, sizeof(content), "%s\n",
             input_event->devname ? input_event->devname : "");
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/phys", input_dir_path);
    snprintf(content, sizeof(content), "%s\n",
             input_event->physloc ? input_event->physloc : "");
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/uniq", input_dir_path);
    snprintf(content, sizeof(content), "%s\n", input_event->uniq);
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/properties", input_dir_path);
    snprintf(content, sizeof(content), "%lx\n", input_event->properties);
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/modalias", input_dir_path);
    snprintf(content, sizeof(content), "input:b%04Xv%04Xp%04Xe%04X\n",
             input_event->inputid.bustype, input_event->inputid.vendor,
             input_event->inputid.product, input_event->inputid.version);
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/id", input_dir_path);
    sysfs_ensure_dir(path);

    snprintf(path, sizeof(path), "%s/id/bustype", input_dir_path);
    snprintf(content, sizeof(content), "%04x\n", input_event->inputid.bustype);
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/id/vendor", input_dir_path);
    snprintf(content, sizeof(content), "%04x\n", input_event->inputid.vendor);
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/id/product", input_dir_path);
    snprintf(content, sizeof(content), "%04x\n", input_event->inputid.product);
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/id/version", input_dir_path);
    snprintf(content, sizeof(content), "%04x\n", input_event->inputid.version);
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/capabilities", input_dir_path);
    sysfs_ensure_dir(path);

    snprintf(path, sizeof(path), "%s/capabilities/ev", input_dir_path);
    input_bitmap_to_string(content, sizeof(content), input_event->evbit,
                           sizeof(input_event->evbit));
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/capabilities/key", input_dir_path);
    input_bitmap_to_string(content, sizeof(content), input_event->keybit,
                           sizeof(input_event->keybit));
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/capabilities/rel", input_dir_path);
    input_bitmap_to_string(content, sizeof(content), input_event->relbit,
                           sizeof(input_event->relbit));
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/capabilities/abs", input_dir_path);
    input_bitmap_to_string(content, sizeof(content), input_event->absbit,
                           sizeof(input_event->absbit));
    input_sysfs_write_file(path, content);

    snprintf(path, sizeof(path), "%s/capabilities/msc", input_dir_path);
    input_sysfs_write_file(path, "0\n");

    snprintf(path, sizeof(path), "%s/capabilities/led", input_dir_path);
    input_sysfs_write_file(path, "0\n");

    snprintf(path, sizeof(path), "%s/capabilities/snd", input_dir_path);
    input_sysfs_write_file(path, "0\n");

    snprintf(path, sizeof(path), "%s/capabilities/sw", input_dir_path);
    input_sysfs_write_file(path, "0\n");

    snprintf(path, sizeof(path), "%s/capabilities/ff", input_dir_path);
    input_sysfs_write_file(path, "0\n");

    if (event_dir_path && event_dir_path[0]) {
        snprintf(path, sizeof(path), "%s/name", event_dir_path);
        snprintf(content, sizeof(content), "%s\n",
                 input_event->devname ? input_event->devname : "");
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/phys", event_dir_path);
        snprintf(content, sizeof(content), "%s\n",
                 input_event->physloc ? input_event->physloc : "");
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/uniq", event_dir_path);
        snprintf(content, sizeof(content), "%s\n", input_event->uniq);
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/properties", event_dir_path);
        snprintf(content, sizeof(content), "%lx\n", input_event->properties);
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/modalias", event_dir_path);
        snprintf(content, sizeof(content), "input:b%04Xv%04Xp%04Xe%04X\n",
                 input_event->inputid.bustype, input_event->inputid.vendor,
                 input_event->inputid.product, input_event->inputid.version);
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/id", event_dir_path);
        sysfs_ensure_dir(path);

        snprintf(path, sizeof(path), "%s/id/bustype", event_dir_path);
        snprintf(content, sizeof(content), "%04x\n",
                 input_event->inputid.bustype);
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/id/vendor", event_dir_path);
        snprintf(content, sizeof(content), "%04x\n",
                 input_event->inputid.vendor);
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/id/product", event_dir_path);
        snprintf(content, sizeof(content), "%04x\n",
                 input_event->inputid.product);
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/id/version", event_dir_path);
        snprintf(content, sizeof(content), "%04x\n",
                 input_event->inputid.version);
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/capabilities", event_dir_path);
        sysfs_ensure_dir(path);

        snprintf(path, sizeof(path), "%s/capabilities/ev", event_dir_path);
        input_bitmap_to_string(content, sizeof(content), input_event->evbit,
                               sizeof(input_event->evbit));
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/capabilities/key", event_dir_path);
        input_bitmap_to_string(content, sizeof(content), input_event->keybit,
                               sizeof(input_event->keybit));
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/capabilities/rel", event_dir_path);
        input_bitmap_to_string(content, sizeof(content), input_event->relbit,
                               sizeof(input_event->relbit));
        input_sysfs_write_file(path, content);

        snprintf(path, sizeof(path), "%s/capabilities/abs", event_dir_path);
        input_bitmap_to_string(content, sizeof(content), input_event->absbit,
                               sizeof(input_event->absbit));
        input_sysfs_write_file(path, content);
    }
}

static size_t input_event_bit(void *data, uint64_t request, void *arg) {
    dev_input_event_t *event = data;

    // Non-evdev ioctls, such as TCGETS2 used to probe for a tty, must fail
    // without being interpreted as EVIOC* requests.
    if (_IOC_TYPE(request) != 'E')
        return (size_t)-ENOTTY;

    size_t number = _IOC_NR(request);
    size_t size = _IOC_SIZE(request);

    if (!arg && size)
        return (size_t)-EFAULT;

    size_t ret = (size_t)-ENOTTY;
    switch (number) {
    case 0x03: {
        struct {
            uint32_t delay;
            uint32_t period;
        } repeat = {
            .delay = 500,
            .period = 33,
        };
        ret = MIN(sizeof(repeat), size);
        if (ret && copy_to_user(arg, &repeat, ret))
            return (size_t)-EFAULT;
        break;
    }
    case 0x20:
        ret = MIN(sizeof(event->evbit), size);
        if (ret && copy_to_user(arg, event->evbit, ret))
            return (size_t)-EFAULT;
        break;
    case (0x20 + EV_KEY):
        ret = MIN(sizeof(event->keybit), size);
        if (ret && copy_to_user(arg, event->keybit, ret))
            return (size_t)-EFAULT;
        break;
    case (0x20 + EV_REL):
        ret = MIN(sizeof(event->relbit), size);
        if (ret && copy_to_user(arg, event->relbit, ret))
            return (size_t)-EFAULT;
        break;
    case (0x20 + EV_ABS):
        ret = MIN(sizeof(event->absbit), size);
        if (ret && copy_to_user(arg, event->absbit, ret))
            return (size_t)-EFAULT;
        break;
    case (0x20 + EV_SW):
    case (0x20 + EV_MSC):
    case (0x20 + EV_SND):
    case (0x20 + EV_LED): {
        size_t value = 0;
        ret = MIN(sizeof(size_t), size);
        if (ret && copy_to_user(arg, &value, ret))
            return (size_t)-EFAULT;
        break;
    }
    case (0x20 + EV_FF): {
        uint8_t zeroes[16] = {0};
        ret = MIN(16, size);
        if (ret && copy_to_user(arg, zeroes, ret))
            return (size_t)-EFAULT;
        break;
    }
    case 0x18: {
        uint8_t map[INPUT_BITMAP_BYTES(KEY_CNT)];
        memset(map, 0, sizeof(map));
        ret = MIN(sizeof(map), size);
        if (ret && copy_to_user(arg, map, ret))
            return (size_t)-EFAULT;
        break;
    }
    case 0x19:
    case 0x1b: {
        size_t value = 0;
        ret = MIN(sizeof(size_t), size);
        if (ret && copy_to_user(arg, &value, ret))
            return (size_t)-EFAULT;
        break;
    }
    default:
        if (number >= 0x40 && number < (0x40 + ABS_CNT)) {
            uint16_t abs = number - 0x40;
            ret = MIN(sizeof(struct input_absinfo), size);
            if (ret && copy_to_user(arg, &event->absinfo[abs], ret))
                return (size_t)-EFAULT;
            break;
        }

        printk("input_event_bit(): Unsupported ioctl: request = %#018lx\n",
               request);
        break;
    }

    return ret;
}

dev_input_event_t *regist_input_dev(const char *device_name,
                                    const input_dev_desc_t *desc) {
    spin_lock(&inputdev_regist_lock);

    char dirname[16];
    sprintf(dirname, "event%d", eventn);

    char input_dirname[16];
    sprintf(input_dirname, "input%d", eventn);

    char dirpath[32];
    sprintf(dirpath, "input/%s", dirname);

    dev_input_event_t *input_event = malloc(sizeof(dev_input_event_t));
    if (!input_event) {
        spin_unlock(&inputdev_regist_lock);
        return NULL;
    }
    memset(input_event, 0, sizeof(dev_input_event_t));
    if (desc) {
        input_event->inputid = desc->inputid;
        input_event->properties = desc->properties;
        memcpy(input_event->evbit, desc->evbit, sizeof(input_event->evbit));
        memcpy(input_event->keybit, desc->keybit, sizeof(input_event->keybit));
        memcpy(input_event->relbit, desc->relbit, sizeof(input_event->relbit));
        memcpy(input_event->absbit, desc->absbit, sizeof(input_event->absbit));
        memcpy(input_event->absinfo, desc->absinfo,
               sizeof(input_event->absinfo));
    }
    if (input_event->inputid.bustype == 0) {
        input_event->inputid.bustype =
            desc && desc->from == INPUT_FROM_USB ? 0x03 : 0x11;
    }
    input_bitmap_set(input_event->evbit, EV_SYN);
    input_event->event_bit = input_event_bit;
    llist_init_head(&input_event->open_files);
    spin_init(&input_event->open_files_lock);
    strncpy(input_event->uniq, device_name, sizeof(input_event->uniq));
    input_event->devname = strdup(device_name);
    input_event->physloc = strdup("");
    if (!input_event->devname || !input_event->physloc) {
        free(input_event->devname);
        free(input_event->physloc);
        free(input_event);
        spin_unlock(&inputdev_regist_lock);
        return NULL;
    }
    uint64_t dev = device_install(DEV_CHAR, DEV_INPUT, input_event, dirpath, 0,
                                  inputdev_open, inputdev_close, inputdev_ioctl,
                                  inputdev_poll, inputdev_event_read,
                                  inputdev_event_write, NULL);
    if (!dev) {
        free(input_event->devname);
        free(input_event->physloc);
        free(input_event);
        spin_unlock(&inputdev_regist_lock);
        return NULL;
    }
    input_event->timesOpened = 0;
    input_event->devnode = NULL;

    char uevent[256];
    char modalias[64];
    const char *extra_uevent =
        desc && desc->uevent_append ? desc->uevent_append : "";
    size_t extra_len = strlen(extra_uevent);
    bool extra_has_trailing_nl =
        extra_len > 0 && extra_uevent[extra_len - 1] == '\n';
    snprintf(modalias, sizeof(modalias), "input:b%04Xv%04Xp%04Xe%04X",
             input_event->inputid.bustype, input_event->inputid.vendor,
             input_event->inputid.product, input_event->inputid.version);
    snprintf(uevent, sizeof(uevent),
             "SUBSYSTEM=input\nID_INPUT=1\n%s%sMODALIAS=%s\n", extra_uevent,
             (extra_len == 0 || extra_has_trailing_nl) ? "" : "\n", modalias);

    char sysfs_path[256];
    char input_dir_path[256];
    char event_parent_path[256];
    char parent_device_path[256];
    memset(sysfs_path, 0, sizeof(sysfs_path));
    memset(input_dir_path, 0, sizeof(input_dir_path));
    memset(event_parent_path, 0, sizeof(event_parent_path));
    memset(parent_device_path, 0, sizeof(parent_device_path));
    if (desc && desc->from == INPUT_FROM_PS2) {
        sprintf(input_dir_path, "/sys/devices/platform/i8042/serio%d/input%d",
                eventn, eventn);
        sprintf(sysfs_path,
                "/sys/devices/platform/i8042/serio%d/input%d/event%d", eventn,
                eventn, eventn);
    } else if (desc && desc->from == INPUT_FROM_USB) {
        if (desc->parent_bus_device && desc->parent_bus_device->sysfs_path) {
            snprintf(parent_device_path, sizeof(parent_device_path), "%s",
                     desc->parent_bus_device->sysfs_path);
            snprintf(input_dir_path, sizeof(input_dir_path), "%s/input/%s",
                     desc->parent_bus_device->sysfs_path, input_dirname);
            snprintf(sysfs_path, sizeof(sysfs_path), "%s/input/%s/%s",
                     desc->parent_bus_device->sysfs_path, input_dirname,
                     dirname);
        } else {
            sprintf(input_dir_path, "/sys/devices/usb/input/%s", input_dirname);
            sprintf(sysfs_path, "/sys/devices/usb/input/%s/%s", input_dirname,
                    dirname);
        }
    }

    char *physloc = strdup(input_dir_path);
    if (physloc) {
        free(input_event->physloc);
        input_event->physloc = physloc;
    }

    snprintf(event_parent_path, sizeof(event_parent_path), "%s",
             input_dir_path);
    input_sysfs_publish_metadata(input_event, input_dir_path, input_dirname,
                                 sysfs_path);

    vfs_node_t *node = sysfs_regist_dev(
        'c', (dev >> 8) & 0xFF, dev & 0xFF, sysfs_path, dirpath, uevent,
        "/sys/class/input", "/sys/class/input", dirname, event_parent_path);
    if (node)
        vfs_iput(node);

    eventn++;

    spin_unlock(&inputdev_regist_lock);

    return input_event;
}
