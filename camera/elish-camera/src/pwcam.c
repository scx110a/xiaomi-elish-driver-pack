/*
 * pwcam.c - publish an elish camera as a real PipeWire "Video/Source" node
 *           with media.role=Camera.
 *
 * Why: WirePlumber 0.4.17's v4l2 monitor publishes the v4l2loopback devices as
 * camera nodes, but those nodes expose no SPA_PARAM_EnumFormat (empty caps), so
 * GStreamer/aperture based apps (GNOME Snapshot, etc.) filter them out
 * ("is_camera()" requires caps intersecting video/x-raw at 0..30 fps).
 * A native PipeWire producer advertises its formats statically, so portal apps
 * see a proper camera and can stream from it.
 *
 * The node is timer-driven at the requested fps.  The camera pipeline calls
 * elish_pwcam_push() with the latest NV12 frame; the node thread copies it into
 * a PipeWire buffer whenever a consumer is streaming.
 */
#include "elish_camera.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>

#include <pipewire/pipewire.h>

#define PWCAM_MAX_BUFFERS 8

struct elish_pwcam {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_source *timer;

    pthread_t thread;
    pthread_mutex_t mu;
    int width, height, fps;
    int stride;

    uint8_t *latest;
    size_t latest_size;
    uint64_t seq;
    uint64_t pushed;
    atomic_bool streaming;
    atomic_bool quit;
    char name[64];
};

static pthread_once_t pw_once = PTHREAD_ONCE_INIT;

static void pw_global_init(void)
{
    pw_init(NULL, NULL);
}

static void on_timeout(void *userdata, uint64_t expirations)
{
    struct elish_pwcam *c = userdata;
    (void)expirations;
    pw_stream_trigger_process(c->stream);
}

static void blank_nv12(uint8_t *p, int stride, int h)
{
    memset(p, 16, (size_t)stride * h);
    memset(p + (size_t)stride * h, 128, (size_t)stride * h / 2);
}

static void on_process(void *userdata)
{
    struct elish_pwcam *c = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    struct spa_meta_header *h;
    uint8_t *p;
    size_t wanted;

    if (!atomic_load(&c->streaming))
        return;
    b = pw_stream_dequeue_buffer(c->stream);
    if (!b)
        return;
    buf = b->buffer;
    if (buf->n_datas < 1 || !(p = buf->datas[0].data)) {
        pw_stream_queue_buffer(c->stream, b);
        return;
    }
    wanted = (size_t)c->stride * c->height * 3 / 2;

    pthread_mutex_lock(&c->mu);
    if (c->latest && c->latest_size >= wanted) {
        for (int y = 0; y < c->height; y++)
            memcpy(p + (size_t)y * c->stride, c->latest + (size_t)y * c->stride, c->stride);
        memcpy(p + (size_t)c->stride * c->height,
               c->latest + (size_t)c->stride * c->height,
               (size_t)c->stride * c->height / 2);
    } else {
        blank_nv12(p, c->stride, c->height);
    }
    c->pushed++;
    pthread_mutex_unlock(&c->mu);

    if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
        h->pts = (int64_t)pw_stream_get_nsec(c->stream);
        h->flags = 0;
        h->seq = (uint32_t)c->pushed;
        h->dts_offset = 0;
    }
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size = (uint32_t)wanted;
    buf->datas[0].chunk->stride = c->stride;
    pw_stream_queue_buffer(c->stream, b);
}

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    struct elish_pwcam *c = userdata;
    (void)old;
    if (error)
        ELISH_ERR("pwcam %s: state %s: %s", c->name, pw_stream_state_as_string(state), error);
    switch (state) {
    case PW_STREAM_STATE_STREAMING: {
        long ns = 1000000000L / (c->fps > 0 ? c->fps : 30);
        struct timespec interval = { ns / 1000000000L, ns % 1000000000L };
        atomic_store(&c->streaming, true);
        pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->timer, &interval, &interval, false);
        break;
    }
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_ERROR:
        atomic_store(&c->streaming, false);
        pw_loop_update_timer(pw_main_loop_get_loop(c->loop), c->timer, NULL, NULL, false);
        break;
    default:
        break;
    }
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
    struct elish_pwcam *c = userdata;
    struct spa_video_info_raw info;
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[2];

    if (!param || id != SPA_PARAM_Format)
        return;
    memset(&info, 0, sizeof(info));
    spa_format_video_raw_parse(param, &info);
    pthread_mutex_lock(&c->mu);
    if ((int)info.size.width != c->width || (int)info.size.height != c->height)
        ELISH_ERR("pwcam %s: consumer negotiated %ux%u (node advertises %dx%d)",
                  c->name, info.size.width, info.size.height, c->width, c->height);
    pthread_mutex_unlock(&c->mu);
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, PWCAM_MAX_BUFFERS),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int((int)((size_t)c->stride * c->height * 3 / 2)),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int(c->stride));
    params[1] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    pw_stream_update_params(c->stream, params, 2);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
};

static void *thread_fn(void *arg)
{
    struct elish_pwcam *c = arg;
    pw_main_loop_run(c->loop);
    return NULL;
}

elish_pwcam *elish_pwcam_create(const char *name, const char *description,
                                const char *location, int width, int height, int fps)
{
    if (width <= 1 || height <= 1 || (width & 1) || (height & 1)) {
        ELISH_ERR("pwcam: NV12 needs even size > 1 (got %dx%d)", width, height);
        return NULL;
    }
    pthread_once(&pw_once, pw_global_init);

    struct elish_pwcam *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->width = width;
    c->height = height;
    c->fps = fps > 0 ? fps : 30;
    c->stride = width;
    atomic_init(&c->streaming, false);
    atomic_init(&c->quit, false);
    pthread_mutex_init(&c->mu, NULL);
    snprintf(c->name, sizeof(c->name), "%s", name);

    c->loop = pw_main_loop_new(NULL);
    if (!c->loop)
        goto err;
    c->context = pw_context_new(pw_main_loop_get_loop(c->loop), NULL, 0);
    if (!c->context)
        goto err;
    c->timer = pw_loop_add_timer(pw_main_loop_get_loop(c->loop), on_timeout, c);
    c->core = pw_context_connect(c->context, NULL, 0);
    if (!c->core) {
        ELISH_ERR("pwcam %s: cannot connect to PipeWire: %s", name, strerror(errno));
        goto err;
    }
    c->stream = pw_stream_new(c->core, name,
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_CLASS, "Video/Source",
            PW_KEY_MEDIA_ROLE, "Camera",
            PW_KEY_NODE_NAME, name,
            PW_KEY_NODE_DESCRIPTION, description,
            PW_KEY_NODE_NICK, description,
            PW_KEY_PRIORITY_SESSION, "1000",
            /* GNOME/aperture based apps read this to decide whether to mirror
             * the selfie preview (front) or not (back). */
            "api.libcamera.location", location,
            NULL));
    if (!c->stream)
        goto err;

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(c->fps, 1)));
    pw_stream_add_listener(c->stream, &c->stream_listener, &stream_events, c);
    if (pw_stream_connect(c->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
                          params, 1) < 0) {
        ELISH_ERR("pwcam %s: pw_stream_connect failed", name);
        goto err;
    }
    if (pthread_create(&c->thread, NULL, thread_fn, c) != 0)
        goto err;
    ELISH_LOG("pwcam: published %s (%s, NV12 %dx%d@%d, media.role=Camera)",
              name, description, width, height, c->fps);
    return c;
err:
    if (c->stream)
        pw_stream_destroy(c->stream);
    if (c->core)
        pw_core_disconnect(c->core);
    if (c->context)
        pw_context_destroy(c->context);
    if (c->loop)
        pw_main_loop_destroy(c->loop);
    pthread_mutex_destroy(&c->mu);
    free(c);
    return NULL;
}

bool elish_pwcam_active(elish_pwcam *c)
{
    return c && atomic_load(&c->streaming);
}

int elish_pwcam_push(elish_pwcam *c, const uint8_t *nv12, int stride)
{
    if (!c || !nv12)
        return -1;
    size_t need = (size_t)c->stride * c->height * 3 / 2;
    pthread_mutex_lock(&c->mu);
    if (c->latest_size < need) {
        uint8_t *n = realloc(c->latest, need);
        if (!n) {
            pthread_mutex_unlock(&c->mu);
            return -1;
        }
        c->latest = n;
        c->latest_size = need;
    }
    if (stride == c->stride) {
        memcpy(c->latest, nv12, need);
    } else {
        for (int y = 0; y < c->height; y++)
            memcpy(c->latest + (size_t)y * c->stride, nv12 + (size_t)y * stride, c->stride);
        const uint8_t *src_uv = nv12 + (size_t)stride * c->height;
        memcpy(c->latest + (size_t)c->stride * c->height, src_uv,
               (size_t)c->stride * c->height / 2);
    }
    c->seq++;
    pthread_mutex_unlock(&c->mu);
    return 0;
}

void elish_pwcam_destroy(elish_pwcam *c)
{
    if (!c)
        return;
    atomic_store(&c->quit, true);
    pw_main_loop_quit(c->loop);
    pthread_join(c->thread, NULL);
    pw_stream_destroy(c->stream);
    pw_core_disconnect(c->core);
    pw_context_destroy(c->context);
    pw_main_loop_destroy(c->loop);
    pthread_mutex_destroy(&c->mu);
    free(c->latest);
    free(c);
}
