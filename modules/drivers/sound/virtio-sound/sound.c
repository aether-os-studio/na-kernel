#include "sound.h"

#include <mod/module.h>
#include <task/task.h>

MODULE_DEPENDS("sound");

#define VIRTIO_SOUND_MAX_STREAMS 32
#define VIRTIO_SOUND_QUEUE_DEPTH SIZE
#define VIRTIO_SOUND_RESPONSE_WATCHDOG_NS 10000000LL
#define VIRTIO_SOUND_IDLE_POLL_NS 1000000LL

typedef struct virtio_sound_tx_slot {
    bool active;
    uint16_t desc_idx;
    void *xfer_hdr;
    void *audio_buf;
    virtio_snd_pcm_status_t *status;
    snd_pcm_uframes_t frames;
} virtio_sound_tx_slot_t;

typedef struct virtio_sound_device virtio_sound_device_t;

typedef struct virtio_sound_stream {
    virtio_sound_device_t *dev;
    sound_pcm_substream_t *substream;
    virtio_snd_pcm_info_t info;
    uint32_t stream_id;
    uint32_t tx_inflight;
    virtio_sound_tx_slot_t tx_slots[VIRTIO_SOUND_QUEUE_DEPTH];
} virtio_sound_stream_t;

struct virtio_sound_device {
    virtio_driver_t *driver;
    sound_card_t *card;
    virtqueue_t *control_vq;
    virtqueue_t *event_vq;
    virtqueue_t *tx_vq;
    virtqueue_t *rx_vq;
    wait_mutex_t control_lock;
    spinlock_t tx_lock;
    uint32_t stream_count;
    virtio_sound_stream_t streams[VIRTIO_SOUND_MAX_STREAMS];
};

static uint16_t virtio_sound_control_wait_used(virtio_sound_device_t *dev,
                                               uint32_t *used_len) {
    for (;;) {
        uint16_t used_idx = virt_queue_get_used_buf(dev->control_vq, used_len);
        if (used_idx != 0xFFFF) {
            return used_idx;
        }

        if (!current_task || !virtio_driver_supports_interrupts(dev->driver)) {
            if (current_task) {
                schedule(0);
            } else {
                arch_pause();
            }
            continue;
        }

        uint64_t observed_seq = virtio_driver_interrupt_seq(dev->driver);
        used_idx = virt_queue_get_used_buf(dev->control_vq, used_len);
        if (used_idx != 0xFFFF) {
            return used_idx;
        }
        (void)virtio_driver_wait_interrupt(dev->driver, observed_seq,
                                           VIRTIO_SOUND_RESPONSE_WATCHDOG_NS);
    }
}

static int virtio_sound_validate(sound_pcm_substream_t *substream) {
    virtio_sound_stream_t *stream = substream ? substream->driver_data : NULL;

    if (!substream || !stream || !stream->dev) {
        return -ENODEV;
    }
    if (stream->substream != substream) {
        return -ENODEV;
    }
    if (!stream->dev->driver || !stream->dev->control_vq ||
        !stream->dev->tx_vq) {
        return -ENODEV;
    }
    return 0;
}

static int virtio_sound_ctl_send(virtio_sound_device_t *dev, const void *req,
                                 size_t req_size, void *resp,
                                 size_t resp_size) {
    void *req_buf = NULL;
    void *resp_buf = NULL;
    virtio_buffer_t bufs[2];
    bool writable[2] = {false, true};
    uint16_t desc_idx = 0xFFFF;
    uint32_t used_len = 0;
    uint16_t used_idx = 0xFFFF;
    int ret = -EIO;

    if (!dev || !req || !resp || !dev->control_vq) {
        return -EINVAL;
    }

    req_buf = alloc_frames_bytes(req_size);
    resp_buf = alloc_frames_bytes(resp_size);
    if (!req_buf || !resp_buf) {
        ret = -ENOMEM;
        goto out;
    }

    memcpy(req_buf, req, req_size);
    memset(resp_buf, 0, resp_size);

    bufs[0].addr = (uint64_t)req_buf;
    bufs[0].size = req_size;
    bufs[1].addr = (uint64_t)resp_buf;
    bufs[1].size = resp_size;

    wait_mutex_lock(&dev->control_lock);
    desc_idx = virt_queue_add_buf(dev->control_vq, bufs, 2, writable);
    if (desc_idx == 0xFFFF) {
        wait_mutex_unlock(&dev->control_lock);
        ret = -EIO;
        goto out;
    }

    virt_queue_submit_buf(dev->control_vq, desc_idx);
    virt_queue_notify(dev->driver, dev->control_vq);

    used_idx = virtio_sound_control_wait_used(dev, &used_len);
    virt_queue_free_desc(dev->control_vq, used_idx);
    wait_mutex_unlock(&dev->control_lock);

    memcpy(resp, resp_buf, resp_size);
    if (((virtio_snd_hdr_t *)resp)->code != VIRTIO_SND_S_OK) {
        ret = -EIO;
        goto out;
    }

    ret = 0;

out:
    if (req_buf) {
        free_frames_bytes(req_buf, req_size);
    }
    if (resp_buf) {
        free_frames_bytes(resp_buf, resp_size);
    }
    return ret;
}

static int virtio_sound_query_pcm_info(virtio_sound_device_t *dev,
                                       virtio_snd_pcm_info_t *infos,
                                       uint32_t count) {
    virtio_snd_query_info_t req;
    struct {
        virtio_snd_hdr_t hdr;
        virtio_snd_pcm_info_t infos[VIRTIO_SOUND_MAX_STREAMS];
    } resp;

    if (!dev || !infos || count > VIRTIO_SOUND_MAX_STREAMS) {
        return -EINVAL;
    }

    memset(&req, 0, sizeof(req));
    req.hdr.code = VIRTIO_SND_R_PCM_INFO;
    req.start_id = 0;
    req.count = count;
    req.size = sizeof(virtio_snd_pcm_info_t);

    memset(&resp, 0, sizeof(resp));
    int ret = virtio_sound_ctl_send(dev, &req, sizeof(req), &resp,
                                    sizeof(virtio_snd_hdr_t) +
                                        count * sizeof(virtio_snd_pcm_info_t));
    if (ret < 0) {
        return ret;
    }

    memcpy(infos, resp.infos, count * sizeof(virtio_snd_pcm_info_t));
    return 0;
}

static uint32_t virtio_sound_rate_to_hz(uint8_t rate) {
    static const uint32_t map[] = {5512,  8000,   11025,  16000, 22050,
                                   32000, 44100,  48000,  64000, 88200,
                                   96000, 176400, 192000, 384000};
    return rate < sizeof(map) / sizeof(map[0]) ? map[rate] : 0;
}

static uint8_t virtio_sound_hz_to_rate(uint32_t hz) {
    static const uint32_t map[] = {5512,  8000,   11025,  16000, 22050,
                                   32000, 44100,  48000,  64000, 88200,
                                   96000, 176400, 192000, 384000};
    for (uint8_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i] == hz) {
            return i;
        }
    }
    return VIRTIO_SND_PCM_RATE_48000;
}

static snd_pcm_format_t virtio_sound_pick_default_format(uint64_t formats) {
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_S16)) {
        return SNDRV_PCM_FORMAT_S16_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_S32)) {
        return SNDRV_PCM_FORMAT_S32_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_S24)) {
        return SNDRV_PCM_FORMAT_S24_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_U8)) {
        return SNDRV_PCM_FORMAT_U8;
    }
    return SNDRV_PCM_FORMAT_S16_LE;
}

static uint32_t virtio_sound_pick_default_rate(uint64_t rates) {
    if (rates & (1ULL << VIRTIO_SND_PCM_RATE_48000)) {
        return 48000;
    }
    if (rates & (1ULL << VIRTIO_SND_PCM_RATE_44100)) {
        return 44100;
    }
    for (uint8_t i = 0; i <= VIRTIO_SND_PCM_RATE_384000; i++) {
        if (rates & (1ULL << i)) {
            return virtio_sound_rate_to_hz(i);
        }
    }
    return 48000;
}

static uint32_t virtio_sound_pick_min_rate(uint64_t rates) {
    for (uint8_t i = 0; i <= VIRTIO_SND_PCM_RATE_384000; i++) {
        if (rates & (1ULL << i)) {
            return virtio_sound_rate_to_hz(i);
        }
    }
    return 48000;
}

static uint32_t virtio_sound_pick_max_rate(uint64_t rates) {
    for (int i = VIRTIO_SND_PCM_RATE_384000; i >= 0; i--) {
        if (rates & (1ULL << i)) {
            return virtio_sound_rate_to_hz((uint8_t)i);
        }
    }
    return 48000;
}

static uint64_t virtio_sound_to_alsa_formats(uint64_t formats) {
    uint64_t result = 0;

    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_S8)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_S8;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_U8)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_U8;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_S16)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_S16_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_U16)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_U16_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_S24_3)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_S24_3LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_U24_3)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_U24_3LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_S24)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_S24_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_U24)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_U24_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_S32)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_S32_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_U32)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_U32_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_FLOAT)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_FLOAT_LE;
    }
    if (formats & (1ULL << VIRTIO_SND_PCM_FMT_FLOAT64)) {
        result |= 1ULL << SNDRV_PCM_FORMAT_FLOAT64_LE;
    }

    return result;
}

static uint32_t virtio_sound_format_bits(snd_pcm_format_t format) {
    switch (format) {
    case SNDRV_PCM_FORMAT_S8:
    case SNDRV_PCM_FORMAT_U8:
        return 8;
    case SNDRV_PCM_FORMAT_S16_LE:
    case SNDRV_PCM_FORMAT_U16_LE:
        return 16;
    case SNDRV_PCM_FORMAT_S24_3LE:
    case SNDRV_PCM_FORMAT_U24_3LE:
    case SNDRV_PCM_FORMAT_S24_LE:
    case SNDRV_PCM_FORMAT_U24_LE:
        return 24;
    case SNDRV_PCM_FORMAT_S32_LE:
    case SNDRV_PCM_FORMAT_U32_LE:
    case SNDRV_PCM_FORMAT_FLOAT_LE:
        return 32;
    case SNDRV_PCM_FORMAT_FLOAT64_LE:
        return 64;
    default:
        return 16;
    }
}

static uint32_t virtio_sound_formats_min_bits(uint64_t formats) {
    uint32_t min_bits = UINT32_MAX;

    for (uint32_t fmt = 0; fmt < 64; fmt++) {
        if (!(formats & (1ULL << fmt))) {
            continue;
        }
        uint32_t bits = virtio_sound_format_bits((snd_pcm_format_t)fmt);
        if (bits && bits < min_bits) {
            min_bits = bits;
        }
    }

    return min_bits == UINT32_MAX ? 16 : min_bits;
}

static uint32_t virtio_sound_formats_max_bits(uint64_t formats) {
    uint32_t max_bits = 0;

    for (uint32_t fmt = 0; fmt < 64; fmt++) {
        if (!(formats & (1ULL << fmt))) {
            continue;
        }
        uint32_t bits = virtio_sound_format_bits((snd_pcm_format_t)fmt);
        if (bits > max_bits) {
            max_bits = bits;
        }
    }

    return max_bits ? max_bits : 16;
}

static uint8_t virtio_sound_from_alsa_format(snd_pcm_format_t format) {
    switch (format) {
    case SNDRV_PCM_FORMAT_S8:
        return VIRTIO_SND_PCM_FMT_S8;
    case SNDRV_PCM_FORMAT_U8:
        return VIRTIO_SND_PCM_FMT_U8;
    case SNDRV_PCM_FORMAT_U16_LE:
        return VIRTIO_SND_PCM_FMT_U16;
    case SNDRV_PCM_FORMAT_S24_3LE:
        return VIRTIO_SND_PCM_FMT_S24_3;
    case SNDRV_PCM_FORMAT_U24_3LE:
        return VIRTIO_SND_PCM_FMT_U24_3;
    case SNDRV_PCM_FORMAT_S24_LE:
        return VIRTIO_SND_PCM_FMT_S24;
    case SNDRV_PCM_FORMAT_U24_LE:
        return VIRTIO_SND_PCM_FMT_U24;
    case SNDRV_PCM_FORMAT_S32_LE:
        return VIRTIO_SND_PCM_FMT_S32;
    case SNDRV_PCM_FORMAT_U32_LE:
        return VIRTIO_SND_PCM_FMT_U32;
    case SNDRV_PCM_FORMAT_FLOAT_LE:
        return VIRTIO_SND_PCM_FMT_FLOAT;
    case SNDRV_PCM_FORMAT_FLOAT64_LE:
        return VIRTIO_SND_PCM_FMT_FLOAT64;
    case SNDRV_PCM_FORMAT_S16_LE:
    default:
        return VIRTIO_SND_PCM_FMT_S16;
    }
}

static void virtio_sound_reclaim_locked(virtio_sound_stream_t *stream) {
    virtio_sound_device_t *dev = stream->dev;
    sound_pcm_substream_t *substream = stream->substream;
    sound_pcm_runtime_t *runtime = &substream->runtime;
    uint32_t used_len = 0;
    uint16_t used_idx = 0xFFFF;

    while ((used_idx = virt_queue_get_used_buf(dev->tx_vq, &used_len)) !=
           0xFFFF) {
        virtio_sound_tx_slot_t *slot = &stream->tx_slots[used_idx];
        if (!slot->active) {
            virt_queue_free_desc(dev->tx_vq, used_idx);
            continue;
        }

        virt_queue_free_desc(dev->tx_vq, used_idx);
        if (slot->status->status == VIRTIO_SND_S_OK) {
            runtime->hw_ptr_base += slot->frames;
            runtime->submitted_frames -=
                MIN(runtime->submitted_frames, slot->frames);
            runtime->delay =
                (snd_pcm_sframes_t)(runtime->appl_ptr - runtime->hw_ptr_base);
        } else {
            runtime->running = false;
            runtime->state = SNDRV_PCM_STATE_XRUN;
        }

        free_frames_bytes(slot->xfer_hdr, sizeof(virtio_snd_pcm_xfer_t));
        free_frames_bytes(slot->audio_buf, runtime->period_bytes);
        free_frames_bytes(slot->status, sizeof(virtio_snd_pcm_status_t));
        memset(slot, 0, sizeof(*slot));
        if (stream->tx_inflight) {
            stream->tx_inflight--;
        }
    }
}

static int virtio_sound_submit_locked(virtio_sound_stream_t *stream) {
    virtio_sound_device_t *dev = stream->dev;
    sound_pcm_substream_t *substream = stream->substream;
    sound_pcm_runtime_t *runtime = &substream->runtime;

    while (runtime->running && runtime->period_size &&
           (runtime->appl_ptr - runtime->hw_ptr_base) >=
               runtime->submitted_frames + runtime->period_size) {
        virtio_sound_tx_slot_t *slot = NULL;
        virtio_buffer_t bufs[3];
        bool writable[3] = {false, false, true};

        void *xfer_hdr = alloc_frames_bytes(sizeof(virtio_snd_pcm_xfer_t));
        void *audio_buf = alloc_frames_bytes(runtime->period_bytes);
        virtio_snd_pcm_status_t *status =
            alloc_frames_bytes(sizeof(virtio_snd_pcm_status_t));
        if (!xfer_hdr || !audio_buf || !status) {
            if (xfer_hdr) {
                free_frames_bytes(xfer_hdr, sizeof(virtio_snd_pcm_xfer_t));
            }
            if (audio_buf) {
                free_frames_bytes(audio_buf, runtime->period_bytes);
            }
            if (status) {
                free_frames_bytes(status, sizeof(virtio_snd_pcm_status_t));
            }
            return -ENOMEM;
        }

        ((virtio_snd_pcm_xfer_t *)xfer_hdr)->stream_id = stream->stream_id;
        memset(status, 0, sizeof(*status));

        snd_pcm_uframes_t start_frame =
            runtime->hw_ptr_base + runtime->submitted_frames;
        uint32_t offset =
            sound_frames_to_bytes(runtime, start_frame % runtime->buffer_size);
        uint32_t first =
            MIN(runtime->period_bytes, runtime->buffer_bytes - offset);
        memcpy(audio_buf, (uint8_t *)runtime->dma_area + offset, first);
        if (runtime->period_bytes > first) {
            memcpy((uint8_t *)audio_buf + first, runtime->dma_area,
                   runtime->period_bytes - first);
        }

        bufs[0].addr = (uint64_t)xfer_hdr;
        bufs[0].size = sizeof(virtio_snd_pcm_xfer_t);
        bufs[1].addr = (uint64_t)audio_buf;
        bufs[1].size = runtime->period_bytes;
        bufs[2].addr = (uint64_t)status;
        bufs[2].size = sizeof(virtio_snd_pcm_status_t);

        uint16_t desc_idx = virt_queue_add_buf(dev->tx_vq, bufs, 3, writable);
        if (desc_idx == 0xFFFF) {
            free_frames_bytes(xfer_hdr, sizeof(virtio_snd_pcm_xfer_t));
            free_frames_bytes(audio_buf, runtime->period_bytes);
            free_frames_bytes(status, sizeof(virtio_snd_pcm_status_t));
            break;
        }

        slot = &stream->tx_slots[desc_idx];
        slot->active = true;
        slot->desc_idx = desc_idx;
        slot->xfer_hdr = xfer_hdr;
        slot->audio_buf = audio_buf;
        slot->status = status;
        slot->frames = runtime->period_size;

        virt_queue_submit_buf(dev->tx_vq, desc_idx);
        virt_queue_notify(dev->driver, dev->tx_vq);
        runtime->submitted_frames += runtime->period_size;
        runtime->delay =
            (snd_pcm_sframes_t)(runtime->appl_ptr - runtime->hw_ptr_base);
        stream->tx_inflight++;
    }

    return 0;
}

static int virtio_sound_set_params(sound_pcm_substream_t *substream) {
    virtio_sound_stream_t *stream = substream->driver_data;
    sound_pcm_runtime_t *runtime = &substream->runtime;
    virtio_snd_pcm_set_params_t req;
    virtio_snd_hdr_t resp;

    memset(&req, 0, sizeof(req));
    req.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    req.hdr.stream_id = stream->stream_id;
    req.buffer_bytes = runtime->buffer_bytes;
    req.period_bytes = runtime->period_bytes;
    req.features = 0;
    req.channels = runtime->channels;
    req.format = virtio_sound_from_alsa_format(runtime->format);
    req.rate = virtio_sound_hz_to_rate(runtime->rate);

    memset(&resp, 0, sizeof(resp));
    return virtio_sound_ctl_send(stream->dev, &req, sizeof(req), &resp,
                                 sizeof(resp));
}

static int virtio_sound_prepare(sound_pcm_substream_t *substream) {
    virtio_sound_stream_t *stream = substream->driver_data;
    virtio_snd_pcm_hdr_t req;
    virtio_snd_hdr_t resp;

    memset(&req, 0, sizeof(req));
    req.hdr.code = VIRTIO_SND_R_PCM_PREPARE;
    req.stream_id = stream->stream_id;
    memset(&resp, 0, sizeof(resp));
    return virtio_sound_ctl_send(stream->dev, &req, sizeof(req), &resp,
                                 sizeof(resp));
}

static int virtio_sound_trigger(sound_pcm_substream_t *substream, bool start) {
    virtio_sound_stream_t *stream = substream->driver_data;
    virtio_snd_pcm_hdr_t req;
    virtio_snd_hdr_t resp;

    memset(&req, 0, sizeof(req));
    req.hdr.code = start ? VIRTIO_SND_R_PCM_START : VIRTIO_SND_R_PCM_STOP;
    req.stream_id = stream->stream_id;
    memset(&resp, 0, sizeof(resp));
    return virtio_sound_ctl_send(stream->dev, &req, sizeof(req), &resp,
                                 sizeof(resp));
}

static int virtio_sound_drain(sound_pcm_substream_t *substream) {
    virtio_sound_stream_t *stream = substream->driver_data;
    sound_pcm_runtime_t *runtime = &substream->runtime;

    spin_lock(&stream->dev->tx_lock);
    virtio_sound_reclaim_locked(stream);
    int ret = virtio_sound_submit_locked(stream);
    if (ret == 0 && (runtime->appl_ptr - runtime->hw_ptr_base) == 0 &&
        stream->tx_inflight == 0) {
        runtime->running = false;
        runtime->draining = false;
        runtime->state = SNDRV_PCM_STATE_SETUP;
    }
    spin_unlock(&stream->dev->tx_lock);
    sound_pcm_notify(substream);
    return ret;
}

static int virtio_sound_free(sound_pcm_substream_t *substream) {
    virtio_sound_stream_t *stream = substream->driver_data;
    virtio_snd_pcm_hdr_t req;
    virtio_snd_hdr_t resp;

    spin_lock(&stream->dev->tx_lock);
    virtio_sound_reclaim_locked(stream);
    spin_unlock(&stream->dev->tx_lock);

    memset(&req, 0, sizeof(req));
    req.hdr.code = VIRTIO_SND_R_PCM_RELEASE;
    req.stream_id = stream->stream_id;
    memset(&resp, 0, sizeof(resp));
    return virtio_sound_ctl_send(stream->dev, &req, sizeof(req), &resp,
                                 sizeof(resp));
}

static int virtio_sound_pump(sound_pcm_substream_t *substream) {
    virtio_sound_stream_t *stream = substream->driver_data;
    sound_pcm_runtime_t *runtime = &substream->runtime;
    int ret = 0;

    spin_lock(&stream->dev->tx_lock);
    virtio_sound_reclaim_locked(stream);
    if (runtime->running) {
        ret = virtio_sound_submit_locked(stream);
        if (ret == 0 && !runtime->draining &&
            (runtime->appl_ptr - runtime->hw_ptr_base) == 0 &&
            stream->tx_inflight == 0) {
            runtime->running = false;
            runtime->state = SNDRV_PCM_STATE_XRUN;
        }
        if (runtime->draining &&
            (runtime->appl_ptr - runtime->hw_ptr_base) == 0 &&
            stream->tx_inflight == 0) {
            runtime->running = false;
            runtime->draining = false;
            runtime->state = SNDRV_PCM_STATE_SETUP;
        }
    }
    spin_unlock(&stream->dev->tx_lock);

    sound_pcm_notify(substream);
    return ret;
}

static sound_pcm_ops_t virtio_sound_pcm_ops = {
    .validate = virtio_sound_validate,
    .set_params = virtio_sound_set_params,
    .prepare = virtio_sound_prepare,
    .trigger = virtio_sound_trigger,
    .drain = virtio_sound_drain,
    .free = virtio_sound_free,
    .pump = virtio_sound_pump,
};

static void virtio_sound_worker(uint64_t arg) {
    virtio_sound_device_t *dev = (virtio_sound_device_t *)arg;

    while (true) {
        uint64_t observed_seq = virtio_driver_interrupt_seq(dev->driver);
        for (uint32_t i = 0; i < dev->stream_count; i++) {
            virtio_sound_stream_t *stream = &dev->streams[i];
            if (!stream->substream) {
                continue;
            }
            spin_lock(&stream->substream->lock);
            virtio_sound_pump(stream->substream);
            spin_unlock(&stream->substream->lock);
        }

        if (!current_task || !virtio_driver_supports_interrupts(dev->driver)) {
            (void)task_block(current_task, TASK_BLOCKING,
                             VIRTIO_SOUND_IDLE_POLL_NS, "virtio_sound");
            continue;
        }
        (void)virtio_driver_wait_interrupt(dev->driver, observed_seq, -1);
    }
}

int virtio_sound_init(virtio_driver_t *driver) {
    virtio_sound_config_t cfg;
    virtio_sound_device_t *dev = NULL;
    uint64_t supported_features = VIRTIO_F_RING_INDIRECT_DESC |
                                  VIRTIO_F_RING_EVENT_IDX | VIRTIO_F_VERSION_1;
    uint64_t features = virtio_begin_init(driver, supported_features);
    sound_pcm_caps_t caps;
    char card_id[16];
    char card_name[32];

    memset(&cfg, 0, sizeof(cfg));
    for (uint32_t i = 0; i < sizeof(cfg) / sizeof(uint32_t); i++) {
        ((uint32_t *)&cfg)[i] =
            driver->op->read_config_space(driver->data, i * sizeof(uint32_t));
    }

    dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return -ENOMEM;
    }

    dev->driver = driver;
    wait_mutex_init(&dev->control_lock);
    spin_init(&dev->tx_lock);
    dev->stream_count = MIN(cfg.streams, (uint32_t)VIRTIO_SOUND_MAX_STREAMS);

    dev->control_vq = virt_queue_new(driver, VIRTIO_SND_VQ_CONTROL,
                                     !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                                     !!(features & VIRTIO_F_RING_EVENT_IDX));
    dev->event_vq = virt_queue_new(driver, VIRTIO_SND_VQ_EVENT,
                                   !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                                   !!(features & VIRTIO_F_RING_EVENT_IDX));
    dev->tx_vq = virt_queue_new(driver, VIRTIO_SND_VQ_TX,
                                !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                                !!(features & VIRTIO_F_RING_EVENT_IDX));
    dev->rx_vq = virt_queue_new(driver, VIRTIO_SND_VQ_RX,
                                !!(features & VIRTIO_F_RING_INDIRECT_DESC),
                                !!(features & VIRTIO_F_RING_EVENT_IDX));
    if (!dev->control_vq || !dev->tx_vq) {
        free(dev);
        return -ENODEV;
    }

    virtio_finish_init(driver);

    if (virtio_sound_query_pcm_info(dev, &dev->streams[0].info,
                                    dev->stream_count) < 0) {
        printk("virtio_sound: failed to query PCM info\n");
        free(dev);
        return -EIO;
    }

    snprintf(card_id, sizeof(card_id), "virtio%u", cfg.streams);
    snprintf(card_name, sizeof(card_name), "VirtIO Sound");
    dev->card = sound_card_create("virtio_snd", card_id, card_name, card_name,
                                  "VirtIO Mixer", "virtio-sound");
    if (!dev->card) {
        free(dev);
        return -ENOMEM;
    }

    for (uint32_t i = 0; i < dev->stream_count; i++) {
        virtio_sound_stream_t *stream = &dev->streams[i];
        sound_pcm_create_info_t create;
        uint32_t dev_minor_base;

        stream->dev = dev;
        stream->stream_id = i;

        memset(&caps, 0, sizeof(caps));
        caps.formats = virtio_sound_to_alsa_formats(stream->info.formats);
        caps.rates = stream->info.rates;
        if (caps.formats == 0) {
            caps.formats = 1ULL << SNDRV_PCM_FORMAT_S16_LE;
        }
        if (caps.rates == 0) {
            caps.rates = 1ULL << VIRTIO_SND_PCM_RATE_48000;
        }
        caps.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
                    SNDRV_PCM_INFO_INTERLEAVED;
        caps.min_rate = virtio_sound_pick_min_rate(caps.rates);
        caps.max_rate = virtio_sound_pick_max_rate(caps.rates);
        caps.channels_min =
            stream->info.channels_min ? stream->info.channels_min : 1;
        caps.channels_max =
            stream->info.channels_max ? stream->info.channels_max : 2;
        caps.sample_bits_min = virtio_sound_formats_min_bits(caps.formats);
        caps.sample_bits_max = virtio_sound_formats_max_bits(caps.formats);
        caps.default_format = virtio_sound_pick_default_format(caps.formats);
        caps.default_rate = virtio_sound_pick_default_rate(caps.rates);
        caps.default_channels =
            MIN(MAX((uint8_t)2, caps.channels_min), caps.channels_max);
        caps.min_period_frames = 64;
        caps.max_period_frames = 8192;
        caps.min_periods = 2;
        caps.max_periods = 32;
        caps.min_buffer_frames = 128;
        caps.max_buffer_frames = 65536;
        caps.fifo_size = 0;

        memset(&create, 0, sizeof(create));
        create.card = dev->card;
        create.device_id = i;
        create.stream = stream->info.direction == VIRTIO_SND_D_INPUT
                            ? SNDRV_PCM_STREAM_CAPTURE
                            : SNDRV_PCM_STREAM_PLAYBACK;
        create.subdevice = 0;
        create.id = "virtio-pcm";
        create.name = stream->info.direction == VIRTIO_SND_D_INPUT
                          ? "VirtIO PCM Capture"
                          : "VirtIO PCM Playback";
        create.subname = "Subdevice #0";
        create.caps = &caps;
        create.ops = &virtio_sound_pcm_ops;
        create.driver_data = stream;
        dev_minor_base = create.stream == SNDRV_PCM_STREAM_PLAYBACK ? 16 : 24;
        create.minor = dev_minor_base + (dev->card->index * 32) + i;
        stream->substream = sound_pcm_create(&create);
    }

    task_create("virtio_snd", virtio_sound_worker, (uint64_t)dev,
                KTHREAD_PRIORITY);
    printk("virtio_sound: initialized %u streams\n", dev->stream_count);
    return 0;
}

static virtio_device_driver_t virtio_sound_driver = {
    .name = "virtio-sound",
    .device_type = VIRTIO_DEVICE_TYPE_SOUND,
    .probe = virtio_sound_init,
    .remove = NULL,
    .shutdown = NULL,
};

int dlmain(void) { return virtio_register_device_driver(&virtio_sound_driver); }
