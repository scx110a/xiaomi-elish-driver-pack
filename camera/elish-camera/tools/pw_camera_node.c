/*
 * pw_camera_node.c - minimal PipeWire camera-source prototype for elish-camera.
 *
 * Publishes an NV12 video source node straight into PipeWire (no v4l2loopback
 * and no WirePlumber camera monitor needed) with:
 *
 *     media.class = "Video/Source"
 *     media.role  = "Camera"
 *
 * That is exactly what xdg-desktop-portal's Camera frontend looks for:
 *   - `org.freedesktop.portal.Camera.IsCameraPresent` becomes TRUE (it scans
 *     the PipeWire registry for nodes with media.role=Camera), and
 *   - `OpenPipeWireRemote` hands sandboxed (Flatpak) apps a restricted
 *     PipeWire fd on which this node is visible, so they can pull frames with
 *     GStreamer `pipewiresrc fd=<fd>` or libpipewire.
 *
 * This is the "方案 B" fallback from docs/architecture.md: a self-published
 * camera node.  On this machine WirePlumber 0.4.17's v4l2 monitor already
 * publishes /dev/video2 as a media.role=Camera node, so this prototype is a
 * proof-of-concept / fallback, not required for the current UI stack.
 *
 * Build (the repo Makefile does not build it because it is optional and needs
 * libpipewire-0.3-dev):
 *
 *   gcc -O2 -Wall -Wextra tools/pw_camera_node.c -o build/pw_camera_node \
 *       $(pkg-config --cflags --libs libpipewire-0.3)
 *
 * Run:
 *
 *   build/pw_camera_node --width 1920 --height 1080 --fps 30 --seconds 0
 *
 * Verify (in another shell):
 *
 *   pw-cli ls Node | grep -A6 elish-camera-pw
 *   gst-launch-1.0 -v pipewiresrc target-object=elish-camera-pw \
 *       num-buffers=90 ! video/x-raw,format=NV12,width=1920,height=1080 \
 *       ! fakesink
 *
 * Based on the PipeWire 1.0.5 `video-src` example (MIT, Wim Taymans).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* locale_t in spa/utils/string.h */
#endif

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>

#include <pipewire/pipewire.h>

#define MAX_BUFFERS 16

struct node_data {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_source *timer;

    struct spa_video_info_raw format;
    uint32_t stride;

    int width;
    int height;
    int fps;
    double seconds; /* 0 = forever */
    const char *name;

    uint64_t start_ns;
    uint64_t frames;
    uint32_t seq;
    bool streaming;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Paint NV12 colour bars + a moving marker; called for every frame. */
static void paint_nv12(uint8_t *y_plane, uint8_t *uv_plane, int w, int h,
                       uint32_t frame)
{
    static const uint8_t bars[8][3] = {
        { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
        { 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 },
    };

    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            int idx = xx * 8 / w;
            if (idx > 7)
                idx = 7;
            y_plane[(size_t)yy * w + xx] = bars[idx][0];
        }
    }
    for (int yy = 0; yy < h / 2; yy++) {
        for (int xx = 0; xx < w; xx += 2) {
            int idx = xx * 8 / w;
            if (idx > 7)
                idx = 7;
            uv_plane[(size_t)yy * w + xx] = bars[idx][1];
            uv_plane[(size_t)yy * w + xx + 1] = bars[idx][2];
        }
    }
    /* white top strip */
    int strip = h / 24 + 1;
    for (int yy = 0; yy < strip && yy < h; yy++)
        memset(y_plane + (size_t)yy * w, 235, (size_t)w);

    /* moving orange column, 32 px wide, moving right */
    int bw = w / 16;
    if (bw < 16)
        bw = 16;
    int x0 = (int)((frame * 16u) % (uint32_t)(w - bw));
    x0 &= ~1;
    for (int yy = strip; yy < h; yy++)
        memset(y_plane + (size_t)yy * w + x0, 180, (size_t)bw);
    for (int yy = 0; yy < h / 2; yy++) {
        for (int xx = x0; xx < x0 + bw && xx + 1 < w; xx += 2) {
            uv_plane[(size_t)yy * w + xx] = 80;
            uv_plane[(size_t)yy * w + xx + 1] = 190;
        }
    }
}

static void on_process(void *userdata)
{
    struct node_data *data = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    struct spa_meta_header *h;
    uint8_t *p;
    size_t payload;

    if (!data->streaming)
        return;

    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        pw_log_warn("pw_camera_node: out of buffers");
        return;
    }
    buf = b->buffer;
    if (buf->n_datas < 1 || (p = buf->datas[0].data) == NULL) {
        pw_stream_queue_buffer(data->stream, b);
        return;
    }

    payload = (size_t)data->stride * (size_t)data->height * 3 / 2;
    if (data->format.format == SPA_VIDEO_FORMAT_NV12) {
        uint8_t *y = p;
        uint8_t *uv = p + (size_t)data->stride * (size_t)data->height;
        paint_nv12(y, uv, data->width, data->height, data->seq);
    } else {
        /* we only negotiate NV12; keep the buffer sane anyway */
        memset(p, 0, payload);
    }

    if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
        h->pts = (int64_t)pw_stream_get_nsec(data->stream);
        h->flags = 0;
        h->seq = data->seq++;
        h->dts_offset = 0;
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size = (uint32_t)payload;
    buf->datas[0].chunk->stride = (int32_t)data->stride;

    data->frames++;
    pw_stream_queue_buffer(data->stream, b);
}

static void on_timeout(void *userdata, uint64_t expirations)
{
    struct node_data *data = userdata;
    (void)expirations;
    pw_stream_trigger_process(data->stream);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
    struct node_data *data = userdata;
    (void)old;

    fprintf(stderr, "pw_camera_node: stream state \"%s\"%s%s\n",
            pw_stream_state_as_string(state),
            error ? ": " : "", error ? error : "");

    switch (state) {
    case PW_STREAM_STATE_ERROR:
    case PW_STREAM_STATE_UNCONNECTED:
        pw_main_loop_quit(data->loop);
        break;
    case PW_STREAM_STATE_PAUSED:
        data->streaming = false;
        pw_loop_update_timer(pw_main_loop_get_loop(data->loop), data->timer,
                             NULL, NULL, false);
        fprintf(stderr, "pw_camera_node: node id %d (media.role=Camera)\n",
                pw_stream_get_node_id(data->stream));
        break;
    case PW_STREAM_STATE_STREAMING: {
        struct timespec timeout = { 0, 1 };
        struct timespec interval;
        int fps = data->fps > 0 ? data->fps : 30;
        long ns = 1000000000L / fps;

        interval.tv_sec = ns / 1000000000L;
        interval.tv_nsec = ns % 1000000000L;
        data->start_ns = now_ns();
        data->frames = 0;
        data->streaming = true;
        pw_loop_update_timer(pw_main_loop_get_loop(data->loop), data->timer,
                             &timeout, &interval, false);
        break;
    }
    default:
        break;
    }
}

static void on_stream_param_changed(void *userdata, uint32_t id,
                                    const struct spa_pod *param)
{
    struct node_data *data = userdata;
    struct pw_stream *stream = data->stream;
    uint8_t params_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer,
                                                    sizeof(params_buffer));
    const struct spa_pod *params[2];

    if (param == NULL || id != SPA_PARAM_Format)
        return;

    spa_format_video_raw_parse(param, &data->format);
    data->stride = (uint32_t)data->format.size.width;
    data->width = (int)data->format.size.width;
    data->height = (int)data->format.size.height;
    fprintf(stderr, "pw_camera_node: negotiated %ux%u format=0x%x\n",
            data->format.size.width, data->format.size.height,
            data->format.format);

    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,
        SPA_POD_Int((int)((size_t)data->stride * data->height * 3 / 2)),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int)data->stride));
    params[1] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

    pw_stream_update_params(stream, params, 2);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
};

static void do_quit(void *userdata, int signal_number)
{
    struct node_data *data = userdata;
    (void)signal_number;
    pw_main_loop_quit(data->loop);
}

/* One-shot timer used by --seconds. */
static void on_quit_timeout(void *userdata, uint64_t expirations)
{
    struct node_data *data = userdata;
    (void)expirations;
    fprintf(stderr, "pw_camera_node: --seconds elapsed, quitting\n");
    pw_main_loop_quit(data->loop);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --width N       frame width (default 1920)\n"
            "  --height N      frame height (default 1080)\n"
            "  --fps N         frame rate (default 30)\n"
            "  --seconds S     quit after S seconds (0 = forever, default 0)\n"
            "  --name NAME     node.name / target-object (default elish-camera-pw)\n"
            "  -h, --help      this help\n",
            argv0);
}

int main(int argc, char *argv[])
{
    struct node_data data = {
        .width = 1920,
        .height = 1080,
        .fps = 30,
        .seconds = 0,
        .name = "elish-camera-pw",
    };
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    int opt;
    static const struct option long_opts[] = {
        { "width", required_argument, NULL, 'W' },
        { "height", required_argument, NULL, 'H' },
        { "fps", required_argument, NULL, 'F' },
        { "seconds", required_argument, NULL, 't' },
        { "name", required_argument, NULL, 'n' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    while ((opt = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'W':
            data.width = atoi(optarg);
            break;
        case 'H':
            data.height = atoi(optarg);
            break;
        case 'F':
            data.fps = atoi(optarg);
            break;
        case 't':
            data.seconds = atof(optarg);
            break;
        case 'n':
            data.name = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 2;
        }
    }
    if (data.width <= 1 || data.height <= 1 || (data.width & 1) || (data.height & 1)) {
        fprintf(stderr, "pw_camera_node: NV12 requires even width/height > 1\n");
        return 2;
    }
    if (data.fps <= 0)
        data.fps = 30;

    /* re-init the builder after the struct copies, then build EnumFormat */
    pw_init(&argc, &argv);

    data.loop = pw_main_loop_new(NULL);
    data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
    data.timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop),
                                   on_timeout, &data);
    data.core = pw_context_connect(data.context, NULL, 0);
    if (data.core == NULL) {
        fprintf(stderr, "pw_camera_node: cannot connect to PipeWire: %s\n",
                strerror(errno));
        return 1;
    }

    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

    data.stream = pw_stream_new(data.core, "elish-camera-pw",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_CLASS, "Video/Source",
            PW_KEY_MEDIA_ROLE, "Camera",
            PW_KEY_NODE_NAME, data.name,
            PW_KEY_NODE_DESCRIPTION, "Elish Camera (PipeWire NV12 prototype)",
            PW_KEY_NODE_NICK, "Elish Camera",
            PW_KEY_PRIORITY_SESSION, "1000",
            NULL));

    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_NV12),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(data.width, data.height)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(data.fps, 1)));

    pw_stream_add_listener(data.stream, &data.stream_listener, &stream_events, &data);
    if (pw_stream_connect(data.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
                          params, 1) < 0) {
        fprintf(stderr, "pw_camera_node: pw_stream_connect failed: %s\n",
                strerror(errno));
        return 1;
    }

    fprintf(stderr, "pw_camera_node: publishing NV12 %dx%d @%dfps as %s "
            "(media.role=Camera)\n", data.width, data.height, data.fps, data.name);

    if (data.seconds > 0) {
        struct pw_loop *loop = pw_main_loop_get_loop(data.loop);
        struct spa_source *quit_timer = pw_loop_add_timer(loop, on_quit_timeout, &data);
        struct timespec ts;
        ts.tv_sec = (time_t)data.seconds;
        ts.tv_nsec = (long)((data.seconds - (double)ts.tv_sec) * 1e9);
        if (quit_timer)
            pw_loop_update_timer(loop, quit_timer, &ts, NULL, false);
    }

    pw_main_loop_run(data.loop);

    fprintf(stderr, "pw_camera_node: stopping (%llu frames pushed)\n",
            (unsigned long long)data.frames);

    pw_stream_destroy(data.stream);
    pw_context_destroy(data.context);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
    return 0;
}
