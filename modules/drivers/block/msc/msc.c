#include "msc.h"
#include <mm/mm.h>

volatile uint64_t usbmsc_drive_id = 0;

#define MSC_CBW_SIGNATURE 0x43425355U
#define MSC_CSW_SIGNATURE 0x53425355U
#define MSC_BOT_RESET 0xFF
#define MSC_BOT_GET_MAX_LUN 0xFE

#define MSC_MAX_LUNS 16
#define MSC_MAX_RETRIES 3
#define MSC_READY_RETRIES 20
#define MSC_READY_DELAY_MS 200
#define MSC_RESET_DELAY_MS 100
#define MSC_MAX_TRANSFER_SIZE (64 * 1024)

static inline uint32_t msc_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t msc_be64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static inline void msc_delay_ms(uint64_t ms) {
    uint64_t timeout = nano_time() + ms * 1000000ULL;
    while (nano_time() < timeout) {
        arch_pause();
    }
}

static int msc_bulk_transfer(usb_msc_device_t *ctrl, bool is_read, void *data,
                             size_t len) {
    usb_pipe_t *pipe;

    if (!ctrl || !ctrl->bulk_in || !ctrl->bulk_out)
        return -EINVAL;

    if (len > 0 && !data)
        return -EINVAL;

    if (len > INT32_MAX)
        return -EINVAL;

    if (len == 0)
        return 0;

    pipe = is_read ? ctrl->bulk_in : ctrl->bulk_out;
    if (usb_send_bulk(pipe, is_read ? USB_DIR_IN : USB_DIR_OUT, data,
                      (int)len) != 0) {
        return -EIO;
    }

    return 0;
}

static int msc_clear_endpoint_halt(usb_msc_device_t *ctrl, usb_pipe_t *pipe) {
    usb_ctrl_request_t req;

    memset(&req, 0, sizeof(req));
    req.bRequestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT;
    req.bRequest = USB_REQ_CLEAR_FEATURE;
    req.wIndex = pipe->ep;

    return usb_send_default_control(ctrl->udev->defpipe, &req, NULL);
}

static void msc_reset_recovery(usb_msc_device_t *ctrl) {
    usb_ctrl_request_t reset_req;

    memset(&reset_req, 0, sizeof(reset_req));
    reset_req.bRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    reset_req.bRequest = MSC_BOT_RESET;
    reset_req.wIndex = ctrl->iface->iface->bInterfaceNumber;

    usb_send_default_control(ctrl->udev->defpipe, &reset_req, NULL);
    msc_clear_endpoint_halt(ctrl, ctrl->bulk_in);
    msc_clear_endpoint_halt(ctrl, ctrl->bulk_out);
    msc_delay_ms(MSC_RESET_DELAY_MS);
}

static int msc_receive_csw(usb_msc_device_t *ctrl, usb_msc_csw_t *csw,
                           uint32_t expected_tag) {
    int ret;

    memset(csw, 0, sizeof(*csw));
    ret = msc_bulk_transfer(ctrl, true, csw, sizeof(*csw));
    if (ret != 0)
        return ret;

    if (csw->dCSWSignature != MSC_CSW_SIGNATURE)
        return -EIO;

    if (csw->dCSWTag != expected_tag)
        return -EIO;

    return 0;
}

static int msc_command_once(usb_msc_device_t *ctrl, uint8_t lun,
                            const void *cmd, uint8_t cmd_len, void *data,
                            size_t data_len, bool is_read) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;
    uint32_t tag;
    int ret = 0;

    if (!ctrl || !cmd || cmd_len == 0 || cmd_len > sizeof(cbw.CBWCB))
        return -EINVAL;

    if (lun >= ctrl->lun_count)
        return -EINVAL;

    if (data_len > 0 && !data)
        return -EINVAL;

    spin_lock(&ctrl->lock);

    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = MSC_CBW_SIGNATURE;
    ctrl->next_tag++;
    if (ctrl->next_tag == 0)
        ctrl->next_tag = 1;
    tag = ctrl->next_tag;

    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = (uint32_t)data_len;
    cbw.bmCBWFlags = (data_len > 0 && is_read) ? USB_DIR_IN : USB_DIR_OUT;
    cbw.bCBWLUN = lun;
    cbw.bCBWCBLength = cmd_len;
    memcpy(cbw.CBWCB, cmd, cmd_len);

    ret = msc_bulk_transfer(ctrl, false, &cbw, sizeof(cbw));
    if (ret != 0)
        goto transport_error;

    if (data_len > 0) {
        ret = msc_bulk_transfer(ctrl, is_read, data, data_len);
        if (ret != 0)
            goto transport_error;
    }

    ret = msc_receive_csw(ctrl, &csw, tag);
    if (ret != 0)
        goto transport_error;

    spin_unlock(&ctrl->lock);

    if (csw.bCSWStatus == 0)
        return 0;
    if (csw.bCSWStatus == 1)
        return -EIO;

    msc_reset_recovery(ctrl);
    return -EAGAIN;

transport_error:
    spin_unlock(&ctrl->lock);
    msc_reset_recovery(ctrl);
    return -EAGAIN;
}

static int msc_command(usb_msc_device_t *ctrl, uint8_t lun, const void *cmd,
                       uint8_t cmd_len, void *data, size_t data_len,
                       bool is_read) {
    int ret;

    for (int i = 0; i < MSC_MAX_RETRIES; i++) {
        ret =
            msc_command_once(ctrl, lun, cmd, cmd_len, data, data_len, is_read);
        if (ret == 0 || ret == -EIO)
            return ret;
    }

    return -EIO;
}

static int msc_test_unit_ready(usb_msc_lun_t *lun) {
    uint8_t cmd[6] = {0x00, 0, 0, 0, 0, 0};
    return msc_command(lun->ctrl, lun->lun, cmd, sizeof(cmd), NULL, 0, true);
}

static int usb_msc_flush(void *data) {
    usb_msc_lun_t *lun = data;
    uint8_t cmd[10] = {0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    return msc_command(lun->ctrl, lun->lun, cmd, sizeof(cmd), NULL, 0, false);
}

static int msc_request_sense(usb_msc_lun_t *lun, uint8_t *sense) {
    uint8_t cmd[6] = {0x03, 0, 0, 0, 18, 0};
    return msc_command(lun->ctrl, lun->lun, cmd, sizeof(cmd), sense, 18, true);
}

static int msc_inquiry(usb_msc_lun_t *lun) {
    uint8_t cmd[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t data[36];

    memset(data, 0, sizeof(data));
    if (msc_command(lun->ctrl, lun->lun, cmd, sizeof(cmd), data, sizeof(data),
                    true) != 0) {
        return -EIO;
    }

    printk("MSC: LUN%u Vendor %.8s Product %.16s Rev %.4s\n", lun->lun,
           &data[8], &data[16], &data[32]);
    return 0;
}

static int msc_read_capacity(usb_msc_lun_t *lun) {
    uint8_t cmd10[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t cap10[8];

    if (msc_command(lun->ctrl, lun->lun, cmd10, sizeof(cmd10), cap10,
                    sizeof(cap10), true) == 0) {
        uint32_t last_lba = msc_be32(&cap10[0]);
        uint32_t block_size = msc_be32(&cap10[4]);

        if (last_lba != 0xFFFFFFFFU && block_size != 0) {
            lun->block_count = (uint64_t)last_lba + 1;
            lun->block_size = block_size;
            return 0;
        }
    }

    {
        uint8_t cmd16[16] = {0x9E, 0x10, 0, 0, 0, 0,  0, 0,
                             0,    0,    0, 0, 0, 32, 0, 0};
        uint8_t cap16[32];
        if (msc_command(lun->ctrl, lun->lun, cmd16, sizeof(cmd16), cap16,
                        sizeof(cap16), true) == 0) {
            uint64_t last_lba = msc_be64(&cap16[0]);
            uint32_t block_size = msc_be32(&cap16[8]);
            if (block_size != 0) {
                lun->block_count = last_lba + 1;
                lun->block_size = block_size;
                return 0;
            }
        }
    }

    return -EIO;
}

static int msc_wait_ready(usb_msc_lun_t *lun) {
    uint8_t sense[18];

    for (int i = 0; i < MSC_READY_RETRIES; i++) {
        if (msc_test_unit_ready(lun) == 0)
            return 0;

        memset(sense, 0, sizeof(sense));
        if (msc_request_sense(lun, sense) == 0) {
            uint8_t key = sense[2] & 0x0f;
            uint8_t asc = sense[12];

            if (key == 0x02 && asc == 0x3A)
                return -ENODEV;
        }

        msc_delay_ms(MSC_READY_DELAY_MS);
    }

    return -EIO;
}

static uint64_t msc_rw_blocks(usb_msc_lun_t *lun, uint64_t lba, void *buf,
                              uint64_t count, bool is_read) {
    uint64_t transferred = 0;
    uint64_t max_blocks;
    uint8_t *ptr = (uint8_t *)buf;

    if (!lun || !buf || count == 0 || lun->block_size == 0)
        return 0;

    if (lba >= lun->block_count)
        return 0;

    if (lba + count > lun->block_count)
        count = lun->block_count - lba;

    max_blocks = MSC_MAX_TRANSFER_SIZE / lun->block_size;
    if (max_blocks == 0)
        max_blocks = 1;

    while (count > 0) {
        uint64_t chunk = count;
        size_t byte_count;
        uint8_t cmd[16];
        int cmd_len;

        if (chunk > max_blocks)
            chunk = max_blocks;

        memset(cmd, 0, sizeof(cmd));
        if (lba <= 0xFFFFFFFFULL && chunk <= 0xFFFFU) {
            cmd_len = 10;
            cmd[0] = is_read ? 0x28 : 0x2A;
            cmd[2] = (uint8_t)(lba >> 24);
            cmd[3] = (uint8_t)(lba >> 16);
            cmd[4] = (uint8_t)(lba >> 8);
            cmd[5] = (uint8_t)lba;
            cmd[7] = (uint8_t)(chunk >> 8);
            cmd[8] = (uint8_t)chunk;
        } else {
            if (chunk > 0xFFFFFFFFULL)
                chunk = 0xFFFFFFFFULL;

            cmd_len = 16;
            cmd[0] = is_read ? 0x88 : 0x8A;
            cmd[2] = (uint8_t)(lba >> 56);
            cmd[3] = (uint8_t)(lba >> 48);
            cmd[4] = (uint8_t)(lba >> 40);
            cmd[5] = (uint8_t)(lba >> 32);
            cmd[6] = (uint8_t)(lba >> 24);
            cmd[7] = (uint8_t)(lba >> 16);
            cmd[8] = (uint8_t)(lba >> 8);
            cmd[9] = (uint8_t)lba;
            cmd[10] = (uint8_t)(chunk >> 24);
            cmd[11] = (uint8_t)(chunk >> 16);
            cmd[12] = (uint8_t)(chunk >> 8);
            cmd[13] = (uint8_t)chunk;
        }

        if (chunk > SIZE_MAX / lun->block_size)
            break;

        byte_count = (size_t)(chunk * lun->block_size);
        if (msc_command(lun->ctrl, lun->lun, cmd, (uint8_t)cmd_len, ptr,
                        byte_count, is_read) != 0) {
            break;
        }

        ptr += byte_count;
        lba += chunk;
        count -= chunk;
        transferred += chunk;
    }

    return transferred;
}

uint64_t usb_msc_read_blocks(void *dev_ptr, uint64_t lba, void *buf,
                             uint64_t count) {
    return msc_rw_blocks((usb_msc_lun_t *)dev_ptr, lba, buf, count, true);
}

uint64_t usb_msc_write_blocks(void *dev_ptr, uint64_t lba, void *buf,
                              uint64_t count) {
    return msc_rw_blocks((usb_msc_lun_t *)dev_ptr, lba, buf, count, false);
}

static uint8_t msc_get_max_lun(usb_msc_device_t *ctrl) {
    usb_ctrl_request_t req;
    uint8_t max_lun = 0;

    memset(&req, 0, sizeof(req));
    req.bRequestType = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    req.bRequest = MSC_BOT_GET_MAX_LUN;
    req.wIndex = ctrl->iface->iface->bInterfaceNumber;
    req.wLength = 1;

    if (usb_send_default_control(ctrl->udev->defpipe, &req, &max_lun) != 0)
        return 0;

    if (max_lun >= MSC_MAX_LUNS)
        max_lun = MSC_MAX_LUNS - 1;

    return max_lun;
}

static int msc_probe_lun(usb_msc_lun_t *lun) {
    char name[32];

    if (msc_wait_ready(lun) != 0) {
        printk("MSC: LUN%u is not ready\n", lun->lun);
        return -EIO;
    }

    msc_inquiry(lun);

    if (msc_read_capacity(lun) != 0 || lun->block_count == 0 ||
        lun->block_size == 0) {
        printk("MSC: LUN%u capacity detection failed\n", lun->lun);
        return -EIO;
    }

    memset(name, 0, sizeof(name));
    sprintf(name, "usbmsc%dl%d", usbmsc_drive_id++, lun->lun);
    regist_blkdev(name, lun, lun->block_size,
                  lun->block_count * lun->block_size, MSC_MAX_TRANSFER_SIZE,
                  usb_msc_read_blocks, usb_msc_write_blocks, usb_msc_flush);

    lun->registered = true;
    printk("MSC: Registered LUN%u, block_size=%u, blocks=%lu\n", lun->lun,
           lun->block_size, lun->block_count);
    return 0;
}

int usb_msc_setup(usb_device_t *usbdev, usb_device_interface_t *iface) {
    if (iface->iface->bInterfaceProtocol != 0x50)
        return -1;

    usb_msc_device_t *ctrl;
    usb_endpoint_descriptor_t *indesc;
    usb_endpoint_descriptor_t *outdesc;
    usb_super_speed_endpoint_descriptor_t *ss_desc;
    uint8_t max_lun;
    bool has_registered_lun = false;

    printk("MSC: Initializing device\n");

    ctrl = malloc(sizeof(*ctrl));
    if (!ctrl)
        return -ENOMEM;

    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->udev = usbdev;
    ctrl->iface = iface;
    ctrl->lock = SPIN_INIT;
    ctrl->next_tag = 1;

    indesc = usb_find_desc(iface, USB_ENDPOINT_XFER_BULK, USB_DIR_IN);
    outdesc = usb_find_desc(iface, USB_ENDPOINT_XFER_BULK, USB_DIR_OUT);
    ss_desc = usb_find_ss_desc(iface);

    if (!indesc || !outdesc)
        goto fail;

    ctrl->bulk_in = usb_alloc_pipe(usbdev, indesc, ss_desc);
    ctrl->bulk_out = usb_alloc_pipe(usbdev, outdesc, ss_desc);
    if (!ctrl->bulk_in || !ctrl->bulk_out)
        goto fail;

    usbdev->desc = ctrl;

    max_lun = msc_get_max_lun(ctrl);
    ctrl->lun_count = max_lun + 1;
    ctrl->luns = malloc(sizeof(*ctrl->luns) * ctrl->lun_count);
    if (!ctrl->luns)
        goto fail;

    memset(ctrl->luns, 0, sizeof(*ctrl->luns) * ctrl->lun_count);
    printk("MSC: Device reports %u LUN(s)\n", ctrl->lun_count);

    for (uint8_t lun = 0; lun < ctrl->lun_count; lun++) {
        ctrl->luns[lun].ctrl = ctrl;
        ctrl->luns[lun].lun = lun;

        if (msc_probe_lun(&ctrl->luns[lun]) == 0)
            has_registered_lun = true;
    }

    if (!has_registered_lun)
        goto fail;

    return 0;

fail:
    if (ctrl) {
        if (ctrl->luns) {
            for (uint8_t lun = 0; lun < ctrl->lun_count; lun++) {
                if (ctrl->luns[lun].registered)
                    unregist_blkdev(&ctrl->luns[lun]);
            }
            free(ctrl->luns);
        }
        if (ctrl->bulk_in)
            usb_free_pipe(usbdev, ctrl->bulk_in);
        if (ctrl->bulk_out)
            usb_free_pipe(usbdev, ctrl->bulk_out);
        free(ctrl);
    }
    usbdev->desc = NULL;
    return -1;
}

int usb_msc_remove(usb_device_t *usbdev) {
    usb_msc_device_t *ctrl;

    if (!usbdev || !usbdev->desc)
        return 0;

    ctrl = (usb_msc_device_t *)usbdev->desc;

    if (ctrl->luns) {
        for (uint8_t lun = 0; lun < ctrl->lun_count; lun++) {
            if (ctrl->luns[lun].registered)
                unregist_blkdev(&ctrl->luns[lun]);
        }
        free(ctrl->luns);
    }

    if (ctrl->bulk_in)
        usb_free_pipe(usbdev, ctrl->bulk_in);
    if (ctrl->bulk_out)
        usb_free_pipe(usbdev, ctrl->bulk_out);

    free(ctrl);
    usbdev->desc = NULL;
    return 0;
}

static const usb_device_id_t msc_ids[] = {
    USB_INTERFACE_INFO(USB_CLASS_MASS_STORAGE, US_SC_SCSI, US_PR_BULK),
    {0},
};

usb_driver_t msc_driver = {
    .name = "msc",
    .id_table = msc_ids,
    .priority = 0,
    .probe = usb_msc_setup,
    .remove = usb_msc_remove,
};

int dlmain() {
    regist_usb_driver(&msc_driver);
    return 0;
}
