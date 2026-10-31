/*
 * QEMU AAudio audio driver (Android)
 *
 * Copyright (c) 2026 Scion Project
 *
 * Output only. QEMU's write op fills a single-producer/single-consumer ring;
 * AAudio's data callback (a real-time thread owned by the audio HAL) drains
 * it, the same split as jackaudio.c and pwaudio.c. When the route changes
 * (headphones, Bluetooth) AAudio disconnects the stream; the error callback
 * schedules a bottom half that reopens it from the main loop.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "audio.h"

#define AUDIO_CAP "aaudio"
#include "audio_int.h"

#include <aaudio/AAudio.h>

/* Default ring length when in|out.buffer-length is not given. */
#define AAUDIO_DEFAULT_BUFFER_USECS 50000
/* Retry a failed reopen about once a second (QEMU writes every 10 ms). */
#define AAUDIO_RETRY_WRITES 100

typedef struct AAudioRing
{
    uint8_t* data;
    size_t   size; /* bytes, a multiple of the frame size */
    size_t   rpos; /* consumer only (AAudio thread) */
    size_t   wpos; /* producer only (QEMU) */
    size_t   used; /* atomic */
} AAudioRing;

typedef struct AAudioVoiceOut
{
    HWVoiceOut                         hw;
    AudiodevAaudioPerDirectionOptions* opt;
    AAudioStream*                      stream;
    AAudioRing                         ring;
    struct audsettings                 as; /* what the stream was opened with */
    size_t                             bytes_per_frame;
    QEMUBH*                            reopen_bh;
    bool                               enabled;      /* atomic, read by the callback */
    bool                               disconnected; /* atomic, set by the error callback */
    unsigned                           retry;
    uint64_t                           underruns; /* atomic, frames of silence inserted */
} AAudioVoiceOut;

static void aaudio_ring_alloc(AAudioRing* r, size_t size)
{
    r->data = g_malloc0(size);
    r->size = size;
    r->rpos = r->wpos = 0;
    qatomic_set(&r->used, 0);
}

static void aaudio_ring_free(AAudioRing* r)
{
    g_free(r->data);
    r->data = NULL;
    r->size = 0;
}

/* Only call while no stream is running (no consumer). */
static void aaudio_ring_clear(AAudioRing* r)
{
    r->rpos = r->wpos = 0;
    qatomic_set(&r->used, 0);
}

static size_t aaudio_ring_free_bytes(AAudioRing* r) { return r->size - qatomic_load_acquire(&r->used); }

static size_t aaudio_ring_write(AAudioRing* r, const uint8_t* src, size_t len)
{
    size_t n = MIN(len, aaudio_ring_free_bytes(r));
    size_t first = MIN(n, r->size - r->wpos);

    memcpy(r->data + r->wpos, src, first);
    memcpy(r->data, src + first, n - first);
    r->wpos = (r->wpos + n) % r->size;
    /* qatomic_add is a full barrier: the data is visible before the count. */
    qatomic_add(&r->used, n);
    return n;
}

static size_t aaudio_ring_read(AAudioRing* r, uint8_t* dst, size_t len)
{
    size_t n = MIN(len, qatomic_load_acquire(&r->used));
    size_t first = MIN(n, r->size - r->rpos);

    memcpy(dst, r->data + r->rpos, first);
    memcpy(dst + first, r->data, n - first);
    r->rpos = (r->rpos + n) % r->size;
    qatomic_sub(&r->used, n);
    return n;
}

/* Runs on AAudio's real-time thread: no locks, no allocation, no logging. */
static aaudio_data_callback_result_t aaudio_data_cb(AAudioStream* stream, void* opaque, void* audio_data,
                                                    int32_t num_frames)
{
    AAudioVoiceOut* vo   = opaque;
    size_t          want = (size_t)num_frames * vo->bytes_per_frame;
    size_t          got  = 0;

    if (likely(qatomic_read(&vo->enabled))) { got = aaudio_ring_read(&vo->ring, audio_data, want); }
    if (got < want) {
        memset((uint8_t*)audio_data + got, 0, want - got);
        if (qatomic_read(&vo->enabled)) { qatomic_add(&vo->underruns, (want - got) / vo->bytes_per_frame); }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/*
 * Runs on an AAudio thread. The stream must not be closed or reopened here,
 * so hand off to the main loop.
 */
static void aaudio_error_cb(AAudioStream* stream, void* opaque, aaudio_result_t error)
{
    AAudioVoiceOut* vo = opaque;

    if (error == AAUDIO_ERROR_DISCONNECTED) {
        qatomic_set(&vo->disconnected, true);
        qemu_bh_schedule(vo->reopen_bh);
    }
}

static aaudio_format_t aaudio_format_for(AudioFormat fmt, AudioFormat* out)
{
    /* AAudio takes I16 and float everywhere; anything else is converted by mixeng. */
    if (fmt == AUDIO_FORMAT_F32) {
        *out = AUDIO_FORMAT_F32;
        return AAUDIO_FORMAT_PCM_FLOAT;
    }
    *out = AUDIO_FORMAT_S16;
    return AAUDIO_FORMAT_PCM_I16;
}

static void aaudio_close_stream(AAudioVoiceOut* vo)
{
    if (vo->stream) {
        AAudioStream_requestStop(vo->stream);
        /* No callbacks run after close returns. */
        AAudioStream_close(vo->stream);
        vo->stream = NULL;
    }
}

static int aaudio_open_stream(AAudioVoiceOut* vo)
{
    AAudioStreamBuilder* b = NULL;
    AAudioStream*        s = NULL;
    aaudio_result_t      r;
    AudioFormat          qfmt;
    aaudio_format_t      afmt = aaudio_format_for(vo->as.fmt, &qfmt);
    bool low_latency = !vo->opt->has_low_latency || vo->opt->low_latency;
    bool exclusive   = vo->opt->has_exclusive && vo->opt->exclusive;

    r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) {
        dolog("AAudio_createStreamBuilder failed: %s\n", AAudio_convertResultToText(r));
        return -1;
    }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, afmt);
    AAudioStreamBuilder_setChannelCount(b, vo->as.nchannels);
    AAudioStreamBuilder_setSampleRate(b, vo->as.freq);
    AAudioStreamBuilder_setPerformanceMode(b, low_latency ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY
                                                          : AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setSharingMode(b, exclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED);
#if __ANDROID_API__ >= 28
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
#endif
    AAudioStreamBuilder_setDataCallback(b, aaudio_data_cb, vo);
    AAudioStreamBuilder_setErrorCallback(b, aaudio_error_cb, vo);

    r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        dolog("AAudioStreamBuilder_openStream failed: %s\n", AAudio_convertResultToText(r));
        return -1;
    }

    /* The format was fixed at init time; reject a stream that differs. */
    if (AAudioStream_getFormat(s) != afmt || AAudioStream_getChannelCount(s) != vo->as.nchannels ||
        AAudioStream_getSampleRate(s) != vo->as.freq) {
        dolog("stream opened as %d Hz, %d ch, format %d; wanted %d Hz, %d ch, format %d\n",
              AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s), AAudioStream_getFormat(s), vo->as.freq,
              vo->as.nchannels, afmt);
        AAudioStream_close(s);
        return -1;
    }

    vo->stream = s;
    qatomic_set(&vo->disconnected, false);
    return 0;
}

static int aaudio_start(AAudioVoiceOut* vo)
{
    aaudio_result_t r = AAudioStream_requestStart(vo->stream);
    if (r != AAUDIO_OK) {
        dolog("AAudioStream_requestStart failed: %s\n", AAudio_convertResultToText(r));
        return -1;
    }
    return 0;
}

/* Main loop: replace a disconnected (or never opened) stream. */
static void aaudio_reopen(AAudioVoiceOut* vo)
{
    aaudio_close_stream(vo);
    aaudio_ring_clear(&vo->ring);
    if (aaudio_open_stream(vo) != 0) { return; }
    dolog("stream reopened (%d frames per burst)\n", AAudioStream_getFramesPerBurst(vo->stream));
    if (vo->enabled) { aaudio_start(vo); }
}

static void aaudio_reopen_bh(void* opaque)
{
    AAudioVoiceOut* vo = opaque;

    dolog("stream disconnected, reopening\n");
    vo->retry = 0;
    aaudio_reopen(vo);
}

static bool aaudio_running(AAudioVoiceOut* vo) { return vo->stream && !qatomic_read(&vo->disconnected); }

static size_t aaudio_buffer_get_free(HWVoiceOut* hw)
{
    AAudioVoiceOut* vo = (AAudioVoiceOut*)hw;

    /* Without a stream, accept and drop everything so the guest's DMA keeps moving. */
    if (!aaudio_running(vo)) { return vo->ring.size; }
    return aaudio_ring_free_bytes(&vo->ring);
}

static size_t aaudio_write(HWVoiceOut* hw, void* buf, size_t len)
{
    AAudioVoiceOut* vo = (AAudioVoiceOut*)hw;

    if (!aaudio_running(vo)) {
        if (!vo->stream && vo->enabled && ++vo->retry % AAUDIO_RETRY_WRITES == 0) { aaudio_reopen(vo); }
        return len;
    }
    return aaudio_ring_write(&vo->ring, buf, len);
}

static int aaudio_init_out(HWVoiceOut* hw, struct audsettings* as, void* drv_opaque)
{
    AAudioVoiceOut* vo  = (AAudioVoiceOut*)hw;
    Audiodev*       dev = drv_opaque;
    AudioFormat     qfmt;
    size_t          frames;

    vo->opt = dev->u.aaudio.out;
    vo->as  = *as;
    aaudio_format_for(as->fmt, &qfmt);
    vo->as.fmt        = qfmt;
    vo->as.endianness = HOST_BIG_ENDIAN;

    audio_pcm_init_info(&hw->info, &vo->as);
    vo->bytes_per_frame = hw->info.bytes_per_frame;

    frames = audio_buffer_frames(qapi_AudiodevAaudioPerDirectionOptions_base(vo->opt), &vo->as,
                                 AAUDIO_DEFAULT_BUFFER_USECS);
    hw->samples = frames;
    aaudio_ring_alloc(&vo->ring, frames * vo->bytes_per_frame);

    vo->reopen_bh = qemu_bh_new(aaudio_reopen_bh, vo);

    if (aaudio_open_stream(vo) != 0) {
        qemu_bh_delete(vo->reopen_bh);
        aaudio_ring_free(&vo->ring);
        return -1;
    }

    dolog("output %d Hz, %d ch, %s, ring %zu frames, burst %d frames, %s\n", vo->as.freq, vo->as.nchannels,
          qfmt == AUDIO_FORMAT_F32 ? "f32" : "s16", frames, AAudioStream_getFramesPerBurst(vo->stream),
          AAudioStream_getPerformanceMode(vo->stream) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "low latency"
                                                                                            : "normal latency");
    return 0;
}

static void aaudio_fini_out(HWVoiceOut* hw)
{
    AAudioVoiceOut* vo = (AAudioVoiceOut*)hw;

    qemu_bh_delete(vo->reopen_bh);
    vo->reopen_bh = NULL;
    aaudio_close_stream(vo);
    aaudio_ring_free(&vo->ring);
    if (vo->underruns) { dolog("%" PRIu64 " frames of underrun silence\n", qatomic_read(&vo->underruns)); }
}

static void aaudio_enable_out(HWVoiceOut* hw, bool enable)
{
    AAudioVoiceOut* vo = (AAudioVoiceOut*)hw;

    if (vo->enabled == enable) { return; }
    qatomic_set(&vo->enabled, enable);
    if (!aaudio_running(vo)) {
        /* Started by the reopen path once a stream exists again. */
        return;
    }
    if (enable) { aaudio_start(vo); }
    else {
        AAudioStream_requestStop(vo->stream);
    }
}

static void* aaudio_audio_init(Audiodev* dev, Error** errp)
{
    assert(dev->driver == AUDIODEV_DRIVER_AAUDIO);
    return dev;
}

static void aaudio_audio_fini(void* opaque) { }

static struct audio_pcm_ops aaudio_pcm_ops = {.init_out        = aaudio_init_out,
                                              .fini_out        = aaudio_fini_out,
                                              .write           = aaudio_write,
                                              .buffer_get_free = aaudio_buffer_get_free,
                                              .run_buffer_out  = audio_generic_run_buffer_out,
                                              .enable_out      = aaudio_enable_out};

static struct audio_driver aaudio_audio_driver = {.name           = "aaudio",
                                                  .descr          = "Android AAudio",
                                                  .init           = aaudio_audio_init,
                                                  .fini           = aaudio_audio_fini,
                                                  .pcm_ops        = &aaudio_pcm_ops,
                                                  .max_voices_out = 1,
                                                  .max_voices_in  = 0,
                                                  .voice_size_out = sizeof(AAudioVoiceOut),
                                                  .voice_size_in  = 0};

static void register_audio_aaudio(void) { audio_driver_register(&aaudio_audio_driver); }
type_init(register_audio_aaudio);
