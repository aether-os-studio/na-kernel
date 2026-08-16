#include "hid.h"
#include <boot/boot.h>
#include <drivers/logger.h>
#include <fs/dev.h>
#include <fs/vfs/vfs.h>
#include <libs/keys.h>

#define USB_DT_HID 0x21
#define USB_DT_REPORT 0x22

#define HID_ITEM_TYPE_MAIN 0
#define HID_ITEM_TYPE_GLOBAL 1
#define HID_ITEM_TYPE_LOCAL 2

#define HID_MAIN_INPUT 0x08
#define HID_MAIN_COLLECTION 0x0A
#define HID_MAIN_END_COLLECTION 0x0C

#define HID_GLOBAL_USAGE_PAGE 0x00
#define HID_GLOBAL_LOGICAL_MIN 0x01
#define HID_GLOBAL_LOGICAL_MAX 0x02
#define HID_GLOBAL_REPORT_SIZE 0x07
#define HID_GLOBAL_REPORT_ID 0x08
#define HID_GLOBAL_REPORT_COUNT 0x09
#define HID_GLOBAL_PUSH 0x0A
#define HID_GLOBAL_POP 0x0B

#define HID_LOCAL_USAGE 0x00
#define HID_LOCAL_USAGE_MIN 0x01
#define HID_LOCAL_USAGE_MAX 0x02

#define HID_COLLECTION_APPLICATION 0x01

#define HID_INPUT_CONSTANT 0x01
#define HID_INPUT_VARIABLE 0x02
#define HID_INPUT_RELATIVE 0x04

#define HID_MAX_FIELDS 128
#define HID_MAX_USAGES 64
#define HID_GLOBAL_STACK_DEPTH 8
#define HID_COLLECTION_DEPTH 16

typedef enum hid_field_kind {
    HID_FIELD_NONE,
    HID_FIELD_KEY,
    HID_FIELD_REL,
    HID_FIELD_ABS,
    HID_FIELD_HAT,
    HID_FIELD_KEYBOARD_ARRAY,
} hid_field_kind_t;

typedef struct hid_field {
    hid_field_kind_t kind;
    bool emit_scancode;
    uint8_t report_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    uint8_t report_count;
    uint8_t flags;
    uint32_t usage;
    int32_t logical_min;
    int32_t logical_max;
    uint16_t code;
} hid_field_t;

typedef struct hid_global_state {
    uint32_t usage_page;
    int32_t logical_min;
    int32_t logical_max;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
} hid_global_state_t;

typedef struct hid_local_state {
    uint32_t usages[HID_MAX_USAGES];
    uint8_t usage_count;
    bool has_usage_range;
    uint32_t usage_min;
    uint32_t usage_max;
} hid_local_state_t;

typedef struct hid_parser {
    hid_global_state_t global;
    hid_global_state_t stack[HID_GLOBAL_STACK_DEPTH];
    int stack_depth;
    hid_local_state_t local;
    size_t report_bits[256];
    uint32_t collection_usage_page[HID_COLLECTION_DEPTH];
    uint32_t collection_usage[HID_COLLECTION_DEPTH];
    uint8_t collection_type[HID_COLLECTION_DEPTH];
    uint8_t collection_depth;
} hid_parser_t;

typedef struct hid_device {
    usb_device_t *usbdev;
    usb_device_interface_t *iface;
    usb_pipe_t *upipe;
    bus_device_t *bus_device;
    dev_input_event_t *input;
    uint16_t iface_num;
    size_t report_buf_len;
    bool has_report_id;
    bool has_keyboard;
    bool has_relative;
    bool has_absolute;
    bool has_digitizer;
    uint32_t instance_id;
    uint8_t *report_desc;
    uint8_t *report_buf;
    uint8_t *prev_reports;
    hid_field_t fields[HID_MAX_FIELDS];
    size_t field_count;
} hid_device_t;

static attribute_t hid_bus_subsystem_attr = {
    .name = "SUBSYSTEM",
    .value = "hid",
};

static attribute_t *hid_bus_default_attrs[] = {
    &hid_bus_subsystem_attr,
};

static bus_t hid_bus = {
    .name = "hid",
    .devices_path = "/sys/bus/hid/devices",
    .drivers_path = "/sys/bus/hid/drivers",
    .bus_default_attrs = hid_bus_default_attrs,
    .bus_default_attrs_count =
        sizeof(hid_bus_default_attrs) / sizeof(hid_bus_default_attrs[0]),
    .bus_default_bin_attrs = NULL,
    .bus_default_bin_attrs_count = 0,
};

static uint32_t hid_device_seq = 1;

struct hid_class_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    struct {
        uint8_t bDescriptorType;
        uint16_t wDescriptorLength;
    } desc[1];
} __attribute__((packed));

static uint16_t KeyToScanCode[] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x001e, 0x0030, 0x002e, 0x0020, 0x0012,
    0x0021, 0x0022, 0x0023, 0x0017, 0x0024, 0x0025, 0x0026, 0x0032, 0x0031,
    0x0018, 0x0019, 0x0010, 0x0013, 0x001f, 0x0014, 0x0016, 0x002f, 0x0011,
    0x002d, 0x0015, 0x002c, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000a, 0x000b, 0x001c, 0x0001, 0x000e, 0x000f, 0x0039,
    0x000c, 0x000d, 0x001a, 0x001b, 0x002b, 0x0000, 0x0027, 0x0028, 0x0029,
    0x0033, 0x0034, 0x0035, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0057, 0x0058, 0xe037, 0x0046,
    0xe145, 0xe052, 0xe047, 0xe049, 0xe053, 0xe04f, 0xe051, 0xe04d, 0xe04b,
    0xe050, 0xe048, 0x0045, 0xe035, 0x0037, 0x004a, 0x004e, 0xe01c, 0x004f,
    0x0050, 0x0051, 0x004b, 0x004c, 0x004d, 0x0047, 0x0048, 0x0049, 0x0052,
    0x0053};

static uint16_t ModifierToScanCode[] = {0x001d, 0x002a, 0x0038, 0xe05b,
                                        0xe01d, 0x0036, 0xe038, 0xe05c};

static uint32_t hid_item_u32(const uint8_t *data, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; i++)
        value |= (uint32_t)data[i] << (i * 8);
    return value;
}

static int32_t hid_sign_extend(uint32_t value, uint8_t bits) {
    if (bits == 0 || bits >= 32)
        return (int32_t)value;

    uint32_t sign = 1u << (bits - 1);
    if (value & sign)
        value |= ~((1u << bits) - 1);

    return (int32_t)value;
}

static int32_t hid_item_s32(const uint8_t *data, size_t size) {
    return hid_sign_extend(hid_item_u32(data, size), size * 8);
}

static void hid_clear_local(hid_parser_t *parser) {
    memset(&parser->local, 0, sizeof(parser->local));
}

static uint32_t hid_resolve_usage(uint32_t usage_page, uint32_t raw_usage) {
    if (raw_usage > 0xffff)
        return raw_usage;
    return (usage_page << 16) | raw_usage;
}

static uint32_t hid_local_usage_at(const hid_parser_t *parser, size_t index) {
    if (index < parser->local.usage_count)
        return hid_resolve_usage(parser->global.usage_page,
                                 parser->local.usages[index]);

    if (parser->local.has_usage_range) {
        uint32_t usage = parser->local.usage_min + index;
        if (usage <= parser->local.usage_max)
            return hid_resolve_usage(parser->global.usage_page, usage);
    }

    return hid_resolve_usage(parser->global.usage_page, 0);
}

static uint32_t hid_current_application(const hid_parser_t *parser) {
    for (int i = (int)parser->collection_depth - 1; i >= 0; i--) {
        if (parser->collection_type[i] == HID_COLLECTION_APPLICATION)
            return (parser->collection_usage_page[i] << 16) |
                   parser->collection_usage[i];
    }
    return 0;
}

static uint16_t hid_keyboard_usage_to_scancode(uint16_t usage) {
    if (usage >= 0xe0 && usage <= 0xe7)
        return ModifierToScanCode[usage - 0xe0];

    if (usage < (sizeof(KeyToScanCode) / sizeof(KeyToScanCode[0])))
        return KeyToScanCode[usage];

    return 0;
}

static uint16_t hid_keyboard_usage_to_evdev(uint16_t usage) {
    uint16_t scancode = hid_keyboard_usage_to_scancode(usage);
    if (!scancode)
        return 0;

    return evdev_code_from_set1_scancode((uint8_t)scancode,
                                         (scancode & 0xff00) != 0);
}

static uint16_t hid_button_usage_to_evdev(uint16_t usage) {
    switch (usage) {
    case 1:
        return BTN_LEFT;
    case 2:
        return BTN_RIGHT;
    case 3:
        return BTN_MIDDLE;
    case 4:
        return BTN_SIDE;
    case 5:
        return BTN_EXTRA;
    case 6:
        return BTN_FORWARD;
    case 7:
        return BTN_BACK;
    case 8:
        return BTN_TASK;
    default:
        if (usage >= 1 && usage <= 10)
            return BTN_0 + usage - 1;
        return 0;
    }
}

static uint16_t hid_consumer_usage_to_evdev(uint16_t usage) {
    switch (usage) {
    case 0x00b5:
        return KEY_NEXTSONG;
    case 0x00b6:
        return KEY_PREVIOUSSONG;
    case 0x00b7:
        return KEY_STOPCD;
    case 0x00cd:
        return KEY_PLAYPAUSE;
    case 0x00e2:
        return KEY_MUTE;
    case 0x00e9:
        return KEY_VOLUMEUP;
    case 0x00ea:
        return KEY_VOLUMEDOWN;
    case 0x0221:
        return KEY_SEARCH;
    case 0x0223:
        return KEY_HOMEPAGE;
    case 0x0224:
        return KEY_BACK;
    case 0x0225:
        return KEY_FORWARD;
    case 0x0227:
        return KEY_REFRESH;
    default:
        return 0;
    }
}

static int hid_get_device_path(bus_device_t *device, char *buf, size_t max) {
    hid_device_t *hid = device->private_data;
    if (!hid || !hid->iface || !hid->iface->usbdev)
        return -1;

    snprintf(buf, max, "0003:%04X:%04X.%04X", hid->iface->usbdev->vendorid,
             hid->iface->usbdev->productid, hid->instance_id);
    return 0;
}

static void hid_register_bus_device(hid_device_t *hid, uint32_t instance_id) {
    if (!hid || hid->bus_device || !hid->iface || !hid->iface->bus_device)
        return;

    attributes_builder_t *builder = attributes_builder_new();
    char value[256];

    snprintf(value, sizeof(value), "%s/0003:%04X:%04X.%04X",
             hid->iface->bus_device->sysfs_path, hid->iface->usbdev->vendorid,
             hid->iface->usbdev->productid, instance_id);
    char devpath[256];
    snprintf(devpath, sizeof(devpath), "/devices%s", value + 4);
    attributes_builder_append(builder, attribute_new("DEVPATH", devpath));

    snprintf(value, sizeof(value), "0003:%04X:%04X.%04X",
             hid->iface->usbdev->vendorid, hid->iface->usbdev->productid,
             instance_id);
    attributes_builder_append(builder, attribute_new("HID_ID", value));

    char modalias[256];
    snprintf(modalias, sizeof(modalias), "hid:b0003g0001v%08Xp%08X",
             hid->iface->usbdev->vendorid, hid->iface->usbdev->productid);

    attributes_builder_append(builder, attribute_new("MODALIAS", modalias));

    hid->bus_device = bus_device_install_internal(&hid_bus, hid, builder->attrs,
                                                  builder->count, NULL, 0,
                                                  hid_get_device_path);

    free(builder);
}

static void hid_append_uevent_line(char *uevent, size_t size,
                                   const char *line) {
    size_t len = strlen(uevent);
    if (len >= size)
        return;

    snprintf(uevent + len, size - len, "%s\n", line);
}

static void hid_register_keyboard_caps(input_dev_desc_t *desc,
                                       const hid_parser_t *parser) {
    input_dev_desc_set_event(desc, EV_REP);

    if (parser->local.has_usage_range) {
        for (uint32_t usage = parser->local.usage_min;
             usage <= parser->local.usage_max; usage++) {
            uint16_t code = hid_keyboard_usage_to_evdev((uint16_t)usage);
            if (code)
                input_dev_desc_set_key(desc, code);
            if (usage == parser->local.usage_max)
                break;
        }
    }

    for (size_t i = 0; i < parser->local.usage_count; i++) {
        uint32_t raw = hid_local_usage_at(parser, i);
        uint16_t code = hid_keyboard_usage_to_evdev(raw & 0xffff);
        if (code)
            input_dev_desc_set_key(desc, code);
    }
}

static bool hid_add_field(hid_device_t *hid, const hid_field_t *field) {
    if (hid->field_count >= HID_MAX_FIELDS)
        return false;

    hid->fields[hid->field_count++] = *field;
    return true;
}

static bool hid_map_scalar_field(hid_device_t *hid, input_dev_desc_t *desc,
                                 uint32_t app_usage, uint32_t usage,
                                 const hid_parser_t *parser,
                                 hid_field_t *field) {
    uint16_t page = usage >> 16;
    uint16_t id = usage & 0xffff;
    bool relative = (field->flags & HID_INPUT_RELATIVE) != 0;
    uint16_t app_page = app_usage >> 16;

    field->usage = usage;

    if (page == 0x07) {
        uint16_t code = hid_keyboard_usage_to_evdev(id);
        if (!code)
            return false;

        field->kind = HID_FIELD_KEY;
        field->code = code;
        hid->has_keyboard = true;
        input_dev_desc_set_event(desc, EV_REP);
        input_dev_desc_set_key(desc, code);
        return true;
    }

    if (page == 0x09) {
        uint16_t code = hid_button_usage_to_evdev(id);
        if (!code)
            return false;

        field->kind = HID_FIELD_KEY;
        field->code = code;
        hid->has_relative = true;
        input_dev_desc_set_property(desc, INPUT_PROP_POINTER);
        input_dev_desc_set_key(desc, code);
        return true;
    }

    if (page == 0x01) {
        switch (id) {
        case 0x30:
            field->code = relative ? REL_X : ABS_X;
            break;
        case 0x31:
            field->code = relative ? REL_Y : ABS_Y;
            break;
        case 0x32:
            field->code = relative ? REL_Z : ABS_Z;
            break;
        case 0x33:
            field->code = relative ? REL_RX : ABS_RX;
            break;
        case 0x34:
            field->code = relative ? REL_RY : ABS_RY;
            break;
        case 0x35:
            field->code = relative ? REL_RZ : ABS_RZ;
            break;
        case 0x38:
            field->code = relative ? REL_WHEEL : ABS_WHEEL;
            break;
        case 0x39:
            field->kind = HID_FIELD_HAT;
            input_dev_desc_set_abs(desc, ABS_HAT0X, -1, 1);
            input_dev_desc_set_abs(desc, ABS_HAT0Y, -1, 1);
            hid->has_absolute = true;
            input_dev_desc_set_property(desc, INPUT_PROP_POINTER);
            return true;
        case 0x81:
            field->kind = HID_FIELD_KEY;
            field->code = KEY_POWER;
            input_dev_desc_set_key(desc, KEY_POWER);
            return true;
        case 0x82:
            field->kind = HID_FIELD_KEY;
            field->code = KEY_SLEEP;
            input_dev_desc_set_key(desc, KEY_SLEEP);
            return true;
        case 0x83:
            field->kind = HID_FIELD_KEY;
            field->code = KEY_WAKEUP;
            input_dev_desc_set_key(desc, KEY_WAKEUP);
            return true;
        default:
            return false;
        }

        if (relative) {
            field->kind = HID_FIELD_REL;
            hid->has_relative = true;
            input_dev_desc_set_property(desc, INPUT_PROP_POINTER);
            input_dev_desc_set_rel(desc, field->code);
        } else {
            field->kind = HID_FIELD_ABS;
            hid->has_absolute = true;
            input_dev_desc_set_abs(desc, field->code,
                                   parser->global.logical_min,
                                   parser->global.logical_max);
            if (app_page == 0x0d || hid->has_digitizer)
                input_dev_desc_set_property(desc, INPUT_PROP_DIRECT);
            else
                input_dev_desc_set_property(desc, INPUT_PROP_POINTER);
        }

        return true;
    }

    if (page == 0x0d) {
        hid->has_digitizer = true;
        input_dev_desc_set_property(desc, INPUT_PROP_DIRECT);

        switch (id) {
        case 0x30:
            field->kind = HID_FIELD_ABS;
            field->code = ABS_PRESSURE;
            input_dev_desc_set_abs(desc, ABS_PRESSURE,
                                   parser->global.logical_min,
                                   parser->global.logical_max);
            return true;
        case 0x32:
            field->kind = HID_FIELD_KEY;
            field->code = BTN_TOOL_PEN;
            input_dev_desc_set_key(desc, BTN_TOOL_PEN);
            return true;
        case 0x3d:
            field->kind = HID_FIELD_ABS;
            field->code = ABS_TILT_X;
            input_dev_desc_set_abs(desc, ABS_TILT_X, parser->global.logical_min,
                                   parser->global.logical_max);
            return true;
        case 0x3e:
            field->kind = HID_FIELD_ABS;
            field->code = ABS_TILT_Y;
            input_dev_desc_set_abs(desc, ABS_TILT_Y, parser->global.logical_min,
                                   parser->global.logical_max);
            return true;
        case 0x42:
            field->kind = HID_FIELD_KEY;
            field->code = BTN_TOUCH;
            input_dev_desc_set_key(desc, BTN_TOUCH);
            return true;
        case 0x44:
            field->kind = HID_FIELD_KEY;
            field->code = BTN_STYLUS;
            input_dev_desc_set_key(desc, BTN_STYLUS);
            return true;
        case 0x45:
            field->kind = HID_FIELD_KEY;
            field->code = BTN_STYLUS2;
            input_dev_desc_set_key(desc, BTN_STYLUS2);
            return true;
        default:
            return false;
        }
    }

    if (page == 0x0c) {
        uint16_t code = hid_consumer_usage_to_evdev(id);
        if (!code)
            return false;

        field->kind = HID_FIELD_KEY;
        field->code = code;
        input_dev_desc_set_key(desc, code);
        return true;
    }

    return false;
}

static void hid_handle_input_item(hid_device_t *hid, input_dev_desc_t *desc,
                                  hid_parser_t *parser, uint8_t flags) {
    size_t report_bits =
        parser->global.report_size * parser->global.report_count;
    size_t base_offset = parser->report_bits[parser->global.report_id];
    uint32_t app_usage = hid_current_application(parser);

    if (flags & HID_INPUT_CONSTANT) {
        parser->report_bits[parser->global.report_id] += report_bits;
        return;
    }

    if ((flags & HID_INPUT_VARIABLE) == 0 &&
        parser->global.usage_page == 0x07) {
        hid_field_t field = {
            .kind = HID_FIELD_KEYBOARD_ARRAY,
            .report_id = parser->global.report_id,
            .bit_offset = base_offset,
            .bit_size = parser->global.report_size,
            .report_count = parser->global.report_count,
            .flags = flags,
            .logical_min = parser->global.logical_min,
            .logical_max = parser->global.logical_max,
        };

        hid->has_keyboard = true;
        hid_register_keyboard_caps(desc, parser);
        hid_add_field(hid, &field);
        parser->report_bits[parser->global.report_id] += report_bits;
        return;
    }

    if ((flags & HID_INPUT_VARIABLE) != 0) {
        for (uint32_t i = 0; i < parser->global.report_count; i++) {
            hid_field_t field = {
                .kind = HID_FIELD_NONE,
                .report_id = parser->global.report_id,
                .bit_offset = base_offset + i * parser->global.report_size,
                .bit_size = parser->global.report_size,
                .report_count = 1,
                .flags = flags,
                .logical_min = parser->global.logical_min,
                .logical_max = parser->global.logical_max,
            };

            uint32_t usage = hid_local_usage_at(parser, i);
            if (hid_map_scalar_field(hid, desc, app_usage, usage, parser,
                                     &field)) {
                hid_add_field(hid, &field);
            }
        }
    }

    parser->report_bits[parser->global.report_id] += report_bits;
}

static int hid_parse_report_descriptor(hid_device_t *hid,
                                       input_dev_desc_t *desc,
                                       const uint8_t *report_desc,
                                       size_t report_desc_len) {
    hid_parser_t parser;
    memset(&parser, 0, sizeof(parser));

    for (size_t pos = 0; pos < report_desc_len;) {
        uint8_t prefix = report_desc[pos++];

        if (prefix == 0xfe) {
            if (pos + 1 >= report_desc_len)
                break;
            uint8_t long_size = report_desc[pos];
            if (pos + 2 + long_size > report_desc_len)
                break;
            pos += 2 + long_size;
            continue;
        }

        uint8_t size_code = prefix & 0x03;
        uint8_t item_size = size_code == 3 ? 4 : size_code;
        uint8_t item_type = (prefix >> 2) & 0x03;
        uint8_t item_tag = (prefix >> 4) & 0x0f;

        if (pos + item_size > report_desc_len)
            break;

        const uint8_t *data = report_desc + pos;
        uint32_t uval = hid_item_u32(data, item_size);
        int32_t sval = hid_item_s32(data, item_size);
        pos += item_size;

        switch (item_type) {
        case HID_ITEM_TYPE_MAIN:
            if (item_tag == HID_MAIN_INPUT) {
                hid_handle_input_item(hid, desc, &parser, uval & 0xff);
                hid_clear_local(&parser);
            } else if (item_tag == HID_MAIN_COLLECTION) {
                uint32_t usage = hid_local_usage_at(&parser, 0);
                if (parser.collection_depth < HID_COLLECTION_DEPTH) {
                    parser.collection_usage_page[parser.collection_depth] =
                        usage >> 16;
                    parser.collection_usage[parser.collection_depth] =
                        usage & 0xffff;
                    parser.collection_type[parser.collection_depth] =
                        uval & 0xff;
                    parser.collection_depth++;
                }
                hid_clear_local(&parser);
            } else if (item_tag == HID_MAIN_END_COLLECTION) {
                if (parser.collection_depth > 0)
                    parser.collection_depth--;
                hid_clear_local(&parser);
            } else {
                hid_clear_local(&parser);
            }
            break;
        case HID_ITEM_TYPE_GLOBAL:
            switch (item_tag) {
            case HID_GLOBAL_USAGE_PAGE:
                parser.global.usage_page = uval;
                break;
            case HID_GLOBAL_LOGICAL_MIN:
                parser.global.logical_min = sval;
                break;
            case HID_GLOBAL_LOGICAL_MAX:
                parser.global.logical_max = sval;
                break;
            case HID_GLOBAL_REPORT_SIZE:
                parser.global.report_size = uval;
                break;
            case HID_GLOBAL_REPORT_ID:
                parser.global.report_id = uval & 0xff;
                hid->has_report_id = true;
                break;
            case HID_GLOBAL_REPORT_COUNT:
                parser.global.report_count = uval;
                break;
            case HID_GLOBAL_PUSH:
                if (parser.stack_depth < HID_GLOBAL_STACK_DEPTH)
                    parser.stack[parser.stack_depth++] = parser.global;
                break;
            case HID_GLOBAL_POP:
                if (parser.stack_depth > 0)
                    parser.global = parser.stack[--parser.stack_depth];
                break;
            default:
                break;
            }
            break;
        case HID_ITEM_TYPE_LOCAL:
            switch (item_tag) {
            case HID_LOCAL_USAGE:
                if (parser.local.usage_count < HID_MAX_USAGES)
                    parser.local.usages[parser.local.usage_count++] = uval;
                break;
            case HID_LOCAL_USAGE_MIN:
                parser.local.has_usage_range = true;
                parser.local.usage_min = uval;
                break;
            case HID_LOCAL_USAGE_MAX:
                parser.local.has_usage_range = true;
                parser.local.usage_max = uval;
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }

    return hid->field_count > 0 ? 0 : -1;
}

static size_t hid_find_report_desc_len(usb_device_interface_t *iface) {
    uint8_t *ptr = (uint8_t *)iface->iface + iface->iface->bLength;
    uint8_t *end = iface->end;

    while (ptr + 2 <= end) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];
        if (len == 0 || ptr + len > end)
            break;

        if (type == USB_DT_HID) {
            struct hid_class_descriptor *desc =
                (struct hid_class_descriptor *)ptr;
            for (int i = 0; i < desc->bNumDescriptors; i++) {
                if (desc->desc[i].bDescriptorType == USB_DT_REPORT)
                    return desc->desc[i].wDescriptorLength;
            }
        }

        ptr += len;
    }

    return 0;
}

static int hid_get_report_descriptor(usb_device_t *usbdev, uint16_t iface_num,
                                     uint8_t *buf, size_t len) {
    usb_ctrl_request_t req;
    req.bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
    req.bRequest = USB_REQ_GET_DESCRIPTOR;
    req.wValue = (USB_DT_REPORT << 8);
    req.wIndex = iface_num;
    req.wLength = len;
    return usb_send_default_control(usbdev->defpipe, &req, buf);
}

static uint32_t hid_extract_bits(const uint8_t *report, size_t report_len,
                                 size_t bit_offset, uint8_t bit_size) {
    uint32_t value = 0;
    size_t total_bits = report_len * 8;

    for (uint8_t i = 0; i < bit_size; i++) {
        size_t bit = bit_offset + i;
        if (bit >= total_bits)
            break;
        if (report[bit / 8] & (1u << (bit % 8)))
            value |= 1u << i;
    }

    return value;
}

static int32_t hid_get_field_value(const hid_field_t *field,
                                   const uint8_t *report, size_t report_len,
                                   bool has_report_id) {
    size_t bit_offset = field->bit_offset + (has_report_id ? 8 : 0);
    uint32_t raw =
        hid_extract_bits(report, report_len, bit_offset, field->bit_size);

    if (field->logical_min < 0)
        return hid_sign_extend(raw, field->bit_size);

    return (int32_t)raw;
}

static uint8_t *hid_prev_report(hid_device_t *hid, uint8_t report_id) {
    return hid->prev_reports + ((size_t)report_id * hid->report_buf_len);
}

static void hid_decode_hat(const hid_field_t *field, int32_t value, int32_t *x,
                           int32_t *y) {
    int32_t hat = value - field->logical_min;
    *x = 0;
    *y = 0;

    switch (hat) {
    case 0:
        *y = -1;
        break;
    case 1:
        *x = 1;
        *y = -1;
        break;
    case 2:
        *x = 1;
        break;
    case 3:
        *x = 1;
        *y = 1;
        break;
    case 4:
        *y = 1;
        break;
    case 5:
        *x = -1;
        *y = 1;
        break;
    case 6:
        *x = -1;
        break;
    case 7:
        *x = -1;
        *y = -1;
        break;
    default:
        break;
    }
}

static bool hid_process_field(hid_device_t *hid, const hid_field_t *field,
                              const uint8_t *report, const uint8_t *prev_report,
                              uint64_t sec, uint64_t usec) {
    switch (field->kind) {
    case HID_FIELD_KEYBOARD_ARRAY: {
        bool emitted = false;
        uint16_t old_usages[HID_MAX_USAGES] = {0};
        uint16_t new_usages[HID_MAX_USAGES] = {0};
        uint8_t count = MIN(field->report_count, HID_MAX_USAGES);

        for (uint8_t i = 0; i < count; i++) {
            size_t bit_offset = field->bit_offset + i * field->bit_size +
                                (hid->has_report_id ? 8 : 0);
            old_usages[i] = hid_extract_bits(prev_report, hid->report_buf_len,
                                             bit_offset, field->bit_size) &
                            0xffff;
            new_usages[i] = hid_extract_bits(report, hid->report_buf_len,
                                             bit_offset, field->bit_size) &
                            0xffff;
        }

        for (uint8_t i = 0; i < count; i++) {
            uint16_t usage = old_usages[i];
            if (usage == 0 || usage == 1)
                continue;

            bool still_pressed = false;
            for (uint8_t j = 0; j < count; j++) {
                if (new_usages[j] == usage) {
                    still_pressed = true;
                    break;
                }
            }

            if (!still_pressed) {
                uint16_t code = hid_keyboard_usage_to_evdev(usage);
                if (code) {
                    input_generate_event(hid->input, EV_KEY, code, 0, sec,
                                         usec);
                    emitted = true;
                }
            }
        }

        for (uint8_t i = 0; i < count; i++) {
            uint16_t usage = new_usages[i];
            if (usage == 0 || usage == 1)
                continue;

            bool already_pressed = false;
            for (uint8_t j = 0; j < count; j++) {
                if (old_usages[j] == usage) {
                    already_pressed = true;
                    break;
                }
            }

            if (!already_pressed) {
                uint16_t code = hid_keyboard_usage_to_evdev(usage);
                if (code) {
                    input_generate_event(hid->input, EV_KEY, code, 1, sec,
                                         usec);
                    emitted = true;
                }
            }
        }

        return emitted;
    }
    case HID_FIELD_KEY: {
        int32_t old_value = hid_get_field_value(
            field, prev_report, hid->report_buf_len, hid->has_report_id);
        int32_t new_value = hid_get_field_value(
            field, report, hid->report_buf_len, hid->has_report_id);
        bool old_pressed = old_value != 0;
        bool new_pressed = new_value != 0;

        if (old_pressed == new_pressed)
            return false;

        input_generate_event(hid->input, EV_KEY, field->code,
                             new_pressed ? 1 : 0, sec, usec);
        return true;
    }
    case HID_FIELD_REL: {
        int32_t value = hid_get_field_value(field, report, hid->report_buf_len,
                                            hid->has_report_id);
        if (!value)
            return false;

        input_generate_event(hid->input, EV_REL, field->code, value, sec, usec);
        return true;
    }
    case HID_FIELD_ABS: {
        int32_t old_value = hid_get_field_value(
            field, prev_report, hid->report_buf_len, hid->has_report_id);
        int32_t new_value = hid_get_field_value(
            field, report, hid->report_buf_len, hid->has_report_id);
        if (old_value == new_value)
            return false;

        input_generate_event(hid->input, EV_ABS, field->code, new_value, sec,
                             usec);
        return true;
    }
    case HID_FIELD_HAT: {
        int32_t old_value = hid_get_field_value(
            field, prev_report, hid->report_buf_len, hid->has_report_id);
        int32_t new_value = hid_get_field_value(
            field, report, hid->report_buf_len, hid->has_report_id);
        int32_t old_x, old_y, new_x, new_y;
        bool emitted = false;

        hid_decode_hat(field, old_value, &old_x, &old_y);
        hid_decode_hat(field, new_value, &new_x, &new_y);

        if (old_x != new_x) {
            input_generate_event(hid->input, EV_ABS, ABS_HAT0X, new_x, sec,
                                 usec);
            emitted = true;
        }
        if (old_y != new_y) {
            input_generate_event(hid->input, EV_ABS, ABS_HAT0Y, new_y, sec,
                                 usec);
            emitted = true;
        }
        return emitted;
    }
    default:
        return false;
    }
}

static void hid_report_complete(int status, int actual_length, void *user_data);

static int hid_submit_report(hid_device_t *hid) {
    if (!hid || !hid->report_buf || !hid->report_buf_len)
        return -EINVAL;

    memset(hid->report_buf, 0, hid->report_buf_len);
    return usb_send_intr_pipe(hid->upipe, hid->report_buf, hid->report_buf_len,
                              hid_report_complete, hid);
}

static void hid_report_complete(int status, int actual_length,
                                void *user_data) {
    hid_device_t *hid = user_data;
    if (!hid)
        return;

    if ((status == EVENT_SUCCESS || status == EVENT_SHORT_PACKET) &&
        actual_length > 0) {
        uint8_t *report = hid->report_buf;
        uint8_t report_id = hid->has_report_id ? report[0] : 0;
        uint8_t *prev = hid_prev_report(hid, report_id);
        uint64_t now = nano_time();
        uint64_t sec = now / 1000000000;
        uint64_t usec = (now % 1000000000) / 1000;
        bool emitted = false;

        for (size_t i = 0; i < hid->field_count; i++) {
            hid_field_t *field = &hid->fields[i];
            if (field->report_id != report_id)
                continue;
            if (hid_process_field(hid, field, report, prev, sec, usec))
                emitted = true;
        }

        if (emitted) {
            input_generate_event(hid->input, EV_SYN, SYN_REPORT, 0, sec, usec);
        }

        memcpy(prev, report, hid->report_buf_len);
    }

    if (status == EVENT_SUCCESS || status == EVENT_SHORT_PACKET)
        (void)hid_submit_report(hid);
}

static int usb_hid_setup(usb_device_t *usbdev, usb_device_interface_t *iface) {
    usb_endpoint_descriptor_t *epdesc =
        usb_find_desc(iface, USB_ENDPOINT_XFER_INT, USB_DIR_IN);
    if (!epdesc) {
        printk("HID: Cannot find interrupt IN endpoint\n");
        return -1;
    }

    size_t report_desc_len = hid_find_report_desc_len(iface);
    if (!report_desc_len || report_desc_len > 4096) {
        printk("HID: Invalid report descriptor length: %zu\n", report_desc_len);
        return -1;
    }

    hid_device_t *hid = calloc(1, sizeof(hid_device_t));
    if (!hid)
        return -1;

    hid->usbdev = usbdev;
    hid->iface = iface;
    hid->iface_num = iface->iface->bInterfaceNumber;
    hid->instance_id = hid_device_seq++;
    hid->report_buf_len = epdesc->wMaxPacketSize;
    if (!hid->report_buf_len) {
        free(hid);
        return -1;
    }

    hid->upipe = usb_alloc_pipe(usbdev, epdesc, NULL);
    if (!hid->upipe) {
        free(hid);
        return -1;
    }

    hid->report_desc = malloc(report_desc_len);
    if (!hid->report_desc) {
        free(hid);
        return -1;
    }
    memset(hid->report_desc, 0, report_desc_len);
    if (hid_get_report_descriptor(usbdev, hid->iface_num, hid->report_desc,
                                  report_desc_len)) {
        free(hid->report_desc);
        free(hid);
        return -1;
    }

    input_dev_desc_t desc = {0};
    desc.from = INPUT_FROM_USB;
    desc.inputid.bustype = 0x03;
    desc.inputid.vendor = usbdev->vendorid;
    desc.inputid.product = usbdev->productid;
    desc.inputid.version = usbdev->device_desc.bcdDevice;
    input_dev_desc_set_event(&desc, EV_SYN);

    if (hid_parse_report_descriptor(hid, &desc, hid->report_desc,
                                    report_desc_len)) {
        free(hid->report_desc);
        free(hid);
        return -1;
    }

    hid_register_bus_device(hid, hid->instance_id);
    desc.parent_bus_device =
        hid->bus_device
            ? hid->bus_device
            : (iface->bus_device ? iface->bus_device : usbdev->bus_device);

    char uevent[128] = "";
    if (hid->has_keyboard)
        hid_append_uevent_line(uevent, sizeof(uevent), "ID_INPUT_KEYBOARD=1");
    if (hid->has_relative)
        hid_append_uevent_line(uevent, sizeof(uevent), "ID_INPUT_MOUSE=1");
    if (hid->has_digitizer || hid->has_absolute)
        hid_append_uevent_line(uevent, sizeof(uevent), "ID_INPUT_TABLET=1");
    desc.uevent_append = uevent;

    char name[64];
    if (usbdev->product[0]) {
        snprintf(name, sizeof(name), "%s", usbdev->product);
    } else {
        snprintf(name, sizeof(name), "usb-hid-%u", hid->iface_num);
    }

    hid->input = regist_input_dev(name, &desc);
    if (!hid->input) {
        free(hid->report_desc);
        free(hid);
        return -1;
    }

    hid->prev_reports = calloc(256, hid->report_buf_len);
    if (!hid->prev_reports) {
        free(hid->report_desc);
        free(hid);
        return -1;
    }
    hid->report_buf = calloc(1, hid->report_buf_len);
    if (!hid->report_buf) {
        free(hid->prev_reports);
        free(hid->report_desc);
        free(hid);
        return -1;
    }

    printk("Setting up USB HID report device: %s\n", name);

    if (hid_submit_report(hid) != 0)
        return -1;

    return 0;
}

int usb_hid_remove(usb_device_t *usbdev) { return 0; }

static const usb_device_id_t hid_ids[] = {
    {
        .match_flags = USB_DEVICE_ID_MATCH_INT_CLASS,
        .bInterfaceClass = USB_CLASS_HID,
    },
    {0},
};

usb_driver_t hid_driver = {
    .name = "hid",
    .id_table = hid_ids,
    .priority = 0,
    .probe = usb_hid_setup,
    .remove = usb_hid_remove,
};

int dlmain() {
    regist_usb_driver(&hid_driver);

    return 0;
}
