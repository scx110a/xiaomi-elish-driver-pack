/*
 * elish-camera: Qualcomm Venus hardware H.264/HEVC encoder (V4L2 stateful M2M).
 *
 * Direct V4L2 memory-to-memory ioctls on the qcom-venus encoder node
 * (resolved by name, currently /dev/video15 or /dev/video16), no
 * GStreamer/libcamera.  Input is NV12, output is an elementary stream
 * (Annex B) written to a file; elish_mux_mp4() stream-copies that into MP4
 * via the host ffmpeg.
 *
 * Driver facts that shaped this code (see docs/architecture.md §4 and
 * docs/measurements/measurements-encode.md):
 *
 *  - venc_try_fmt_common() aligns luma width to 128 and height to 32, so a
 *    request for 4208x3120 comes back as 4224x3136 (stride 4224) and
 *    3280x2464 comes back as 3328x2464 (stride 3328).  The encoded picture
 *    size is selected with VIDIOC_S_SELECTION (V4L2_SEL_TGT_CROP) on the
 *    OUTPUT queue, which sets the HFI input/output resolution without
 *    requiring the buffer to shrink: we feed stride*out_h*3/2 padded NV12
 *    and crop the top-left width x height pixels.  That is what makes the
 *    bitstream exactly 4208x3120 / 3280x2464 instead of the padded size.
 *  - The codec profile/level controls default to level 1.0, so the SPS must
 *    be fixed up explicitly or ffmpeg/players see a wildly understated
 *    level.  We set the highest level the driver exposes: H.264 5.1
 *    (4208x3120 exceeds level 5.1 MaxFS, see measurements doc) and HEVC 5.1
 *    or 6.2 depending on picture size.
 *  - Force-IDR is done with V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, which only
 *    works once both queues are streaming.
 *  - The encoder firmware is fast enough that the ioctl round trip, not the
 *    encoder, is the throughput limit; 4+4 mmap buffers give the required
 *    pipelining for continuous 30fps without blocking the caller.
 *
 * Environment knobs (for tools/enc_test.c, not required by the public API):
 *   ELISH_ENC_GOP  override GOP size (default: one second = fps)
 *   ELISH_ENC_RC   "cbr" (default) or "vbr" rate control mode
 *   ELISH_MUX_FPS  framerate hint passed to the raw ES demuxer in
 *                  elish_mux_mp4() (default 30); H.264 ES carries VUI
 *                  timing, raw HEVC ES does not.
 */

#include "venus_enc_internal.h"

#include <elish_camera.h>

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define ENC_DEV_PATH "/dev/video16"

/* The Venus encoder/decoder video node numbers are not stable across boots
 * (probe order changes: encoder was /dev/video16 before one reboot and
 * /dev/video15 after it).  Resolve the node by its sysfs name. */
static const char *enc_devnode(void)
{
    static char path[64];
    DIR *d = opendir("/sys/class/video4linux");

    if (d) {
        struct dirent *de;

        while ((de = readdir(d))) {
            char p[512];
            char buf[128];
            FILE *f;

            if (strncmp(de->d_name, "video", 5) != 0)
                continue;
            snprintf(p, sizeof(p), "/sys/class/video4linux/%.32s/name", de->d_name);
            f = fopen(p, "r");
            if (!f)
                continue;
            buf[0] = 0;
            if (fgets(buf, sizeof(buf), f) &&
                strncmp(buf, "qcom-venus-encoder", 18) == 0) {
                fclose(f);
                snprintf(path, sizeof(path), "/dev/%.32s", de->d_name);
                closedir(d);
                return path;
            }
            fclose(f);
        }
        closedir(d);
    }
    return ENC_DEV_PATH;
}

/* Venus is happy with 4 buffers of each kind (probed: STREAMON succeeds and
 * 4K30 never starves); more buffers only waste memory. */
#define ENC_NUM_CAP_BUFS 4
#define ENC_NUM_OUT_BUFS 4
/* Worst-case wait for a free OUTPUT buffer / an incoming capture buffer. */
#define ENC_IO_TIMEOUT_MS 2000

#define ENC_BITRATE_MIN 32000
#define ENC_BITRATE_MAX 160000000
#define ENC_FPS_MAX 960
#define ENC_GOP_MAX 65535

struct enc_buf {
    uint8_t *addr;
    size_t len;
};

struct elish_encoder {
    int fd;
    elish_codec codec;
    uint32_t codec_fourcc;

    int width;  /* requested / encoded picture size (crop) */
    int height;
    int out_w;  /* real OUTPUT frame size from S_FMT (128x32 aligned) */
    int out_h;
    int stride; /* real luma stride from S_FMT */
    size_t in_frame_size; /* stride*out_h*3/2: bytes sent to the firmware */
    size_t cap_buf_len;   /* compressed capture buffer size */

    int fps;
    int gop;
    int bitrate;
    int force_idr_every; /* 0 = off; ELISH_ENC_FORCE_IDR convenience knob */

    FILE *es;
    char *es_path;

    struct enc_buf *cap;
    int ncap;
    struct enc_buf *out;
    int nout;
    int *free_out; /* output buffer indices not yet queued for the first time */
    int nfree;

    unsigned long long pushed;
    unsigned long long encoded;
    unsigned long long keyframes;
    unsigned long long skipped; /* encode frames the firmware did not code */
    unsigned long long bytes;

    bool started;
    bool streamon_out;
    bool streamon_cap;
    bool warned_force_key;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static int enc_ioctl(int fd, unsigned long req, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);

    return ret;
}

static const char *enc_fourcc_str(uint32_t fourcc, char buf[5])
{
    buf[0] = (char)(fourcc & 0xff);
    buf[1] = (char)((fourcc >> 8) & 0xff);
    buf[2] = (char)((fourcc >> 16) & 0xff);
    buf[3] = (char)((fourcc >> 24) & 0xff);
    buf[4] = '\0';
    return buf;
}

static int enc_s_ctrl(struct elish_encoder *enc, unsigned int id, int value,
                      const char *name, bool required)
{
    struct v4l2_control ctrl = { .id = id, .value = value };

    if (enc_ioctl(enc->fd, VIDIOC_S_CTRL, &ctrl) == 0)
        return 0;

    if (required) {
        ELISH_ERR("venus: cannot set %s=%d: %s", name, value, strerror(errno));
        return -1;
    }
    /* Optional controls are informational only; never fail the pipeline. */
    ELISH_ERR("venus: warning: cannot set %s=%d: %s", name, value, strerror(errno));
    return 0;
}

static int enc_set_fmt(struct elish_encoder *enc, enum v4l2_buf_type type,
                       uint32_t fourcc, int width, int height,
                       uint32_t sizeimage, struct v4l2_format *out_fmt)
{
    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;
    fmt.fmt.pix_mp.width = (uint32_t)width;
    fmt.fmt.pix_mp.height = (uint32_t)height;
    fmt.fmt.pix_mp.pixelformat = fourcc;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (sizeimage)
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;

    if (enc_ioctl(enc->fd, VIDIOC_S_FMT, &fmt) < 0) {
        ELISH_ERR("venus: S_FMT type %d (%dx%d, '%s'): %s", (int)type, width,
                  height, enc_fourcc_str(fourcc, (char[5]){0}), strerror(errno));
        return -1;
    }

    if (fmt.fmt.pix_mp.pixelformat != fourcc) {
        char got[5];
        ELISH_ERR("venus: S_FMT type %d returned '%s' instead of '%s'",
                  (int)type, enc_fourcc_str(fmt.fmt.pix_mp.pixelformat, got),
                  enc_fourcc_str(fourcc, (char[5]){0}));
        return -1;
    }

    if (out_fmt)
        *out_fmt = fmt;
    return 0;
}

static int enc_reqbufs(struct elish_encoder *enc, enum v4l2_buf_type type,
                       int count, struct enc_buf **bufs, int *nbufs)
{
    struct v4l2_requestbuffers req;
    struct enc_buf *arr;
    unsigned int i;

    memset(&req, 0, sizeof(req));
    req.count = (uint32_t)count;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;

    if (enc_ioctl(enc->fd, VIDIOC_REQBUFS, &req) < 0) {
        ELISH_ERR("venus: REQBUFS type %d count %d: %s", (int)type, count,
                  strerror(errno));
        return -1;
    }
    if (req.count < 2) {
        ELISH_ERR("venus: REQBUFS type %d returned only %u buffers", (int)type,
                  req.count);
        return -1;
    }

    arr = calloc(req.count, sizeof(*arr));
    if (!arr) {
        ELISH_ERR("venus: out of memory for %u buffers", req.count);
        return -1;
    }

    for (i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        if (enc_ioctl(enc->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ELISH_ERR("venus: QUERYBUF type %d #%u: %s", (int)type, i,
                      strerror(errno));
            goto fail;
        }

        arr[i].len = planes[0].length;
        arr[i].addr = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, enc->fd, (off_t)planes[0].m.mem_offset);
        if (arr[i].addr == MAP_FAILED) {
            arr[i].addr = NULL;
            ELISH_ERR("venus: mmap type %d #%u (%u bytes): %s", (int)type, i,
                      planes[0].length, strerror(errno));
            goto fail;
        }
    }

    *bufs = arr;
    *nbufs = (int)req.count;
    return 0;

fail:
    for (i = 0; i < req.count; i++) {
        if (arr[i].addr)
            munmap(arr[i].addr, arr[i].len);
    }
    free(arr);
    return -1;
}

static void enc_free_bufs(struct elish_encoder *enc, enum v4l2_buf_type type,
                          struct enc_buf *bufs, int nbufs)
{
    struct v4l2_requestbuffers req;
    int i;

    if (!bufs)
        return;

    for (i = 0; i < nbufs; i++) {
        if (bufs[i].addr)
            munmap(bufs[i].addr, bufs[i].len);
    }
    free(bufs);

    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    (void)enc_ioctl(enc->fd, VIDIOC_REQBUFS, &req);
}

static int enc_qbuf(struct elish_encoder *enc, enum v4l2_buf_type type,
                    unsigned int index, size_t bytesused, uint64_t pts_ns)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.m.planes = planes;
    buf.length = 1;
    planes[0].bytesused = (uint32_t)bytesused;
    planes[0].length = (uint32_t)(type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                                      ? enc->in_frame_size
                                      : enc->cap_buf_len);
    buf.timestamp.tv_sec = (time_t)(pts_ns / 1000000000ull);
    buf.timestamp.tv_usec = (long)((pts_ns % 1000000000ull) / 1000ull);

    if (enc_ioctl(enc->fd, VIDIOC_QBUF, &buf) < 0) {
        ELISH_ERR("venus: QBUF type %d #%u: %s", (int)type, index,
                  strerror(errno));
        return -1;
    }
    return 0;
}

/* Non-blocking dequeue; returns buffer index, -1 on EAGAIN, or -2 on error. */
static int enc_dqbuf_try(struct elish_encoder *enc, enum v4l2_buf_type type,
                         struct v4l2_buffer *out_buf,
                         struct v4l2_plane *out_plane)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;

    if (enc_ioctl(enc->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return -1;
        ELISH_ERR("venus: DQBUF type %d: %s", (int)type, strerror(errno));
        return -2;
    }

    if (out_buf)
        *out_buf = buf;
    if (out_plane)
        *out_plane = planes[0];
    return (int)buf.index;
}

/* ------------------------------------------------------------------ */
/* streaming internals                                                */
/* ------------------------------------------------------------------ */

/* Write every encoded frame that is already available back to the ES file.
 * wait_ms > 0 blocks up to that long for at least one frame.
 * Returns 0 on success (including "nothing ready"), -1 on fatal error. */
static int enc_drain_capture(struct elish_encoder *enc, int wait_ms)
{
    for (;;) {
        struct v4l2_buffer buf;
        struct v4l2_plane plane;
        int idx;

        idx = enc_dqbuf_try(enc, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &buf, &plane);
        if (idx == -1) {
            if (wait_ms <= 0)
                return 0;
            struct pollfd pfd = { .fd = enc->fd, .events = POLLIN | POLLERR };
            int pr;

            do {
                pr = poll(&pfd, 1, wait_ms);
            } while (pr == -1 && errno == EINTR);
            if (pr <= 0) {
                if (pr == 0)
                    ELISH_ERR("venus: timeout waiting for an encoded frame");
                else
                    ELISH_ERR("venus: poll: %s", strerror(errno));
                return -1;
            }
            wait_ms = 0;
            continue;
        }
        if (idx < 0)
            return -1;

        if (buf.flags & V4L2_BUF_FLAG_KEYFRAME)
            enc->keyframes++;

        if (enc->es) {
            size_t used = plane.bytesused;

            if (used) {
                if (fwrite(enc->cap[idx].addr, 1, used, enc->es) != used) {
                    ELISH_ERR("venus: short write to %s: %s", enc->es_path,
                              strerror(errno));
                } else {
                    enc->bytes += used;
                }
            } else {
                enc->skipped++;
                if (enc->skipped == 1)
                    ELISH_ERR("venus: warning: firmware returned an empty "
                              "(not coded) frame; content may exceed the "
                              "capture buffer (input too complex)");
            }
            enc->encoded++;
        }

        if (enc_qbuf(enc, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                     (unsigned int)idx, 0, 0) < 0)
            return -1;
    }
}

static int enc_get_free_output(struct elish_encoder *enc)
{
    /* Buffers that have never been queued are immediately usable; the
     * driver cannot dequeue a buffer that was never queued, so keeping this
     * list is what lets the very first push happen. */
    if (enc->nfree > 0)
        return enc->free_out[--enc->nfree];

    for (;;) {
        int idx;

        /* Collect finished frames first, then reclaim a retired input
         * buffer. */
        if (enc_drain_capture(enc, 0) < 0)
            return -1;

        idx = enc_dqbuf_try(enc, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, NULL, NULL);
        if (idx >= 0)
            return idx;
        if (idx == -2)
            return -1;

        {
            struct pollfd pfd = { .fd = enc->fd, .events = POLLIN | POLLERR };
            int pr;

            do {
                pr = poll(&pfd, 1, ENC_IO_TIMEOUT_MS);
            } while (pr == -1 && errno == EINTR);
            if (pr == 0) {
                ELISH_ERR("venus: encoder stalled (no free input buffer in %d ms)",
                          ENC_IO_TIMEOUT_MS);
                return -1;
            }
            if (pr < 0) {
                ELISH_ERR("venus: poll: %s", strerror(errno));
                return -1;
            }
        }
    }
}

/* Copy a caller NV12 frame into the padded Venus input buffer: luma rows at
 * the top, UV plane at stride*out_h, both cropped to width x height. */
static void enc_copy_nv12(struct elish_encoder *enc, uint8_t *dst,
                          const uint8_t *src, int src_stride)
{
    const int w = enc->width;
    const int h = enc->height;
    const size_t dst_luma = (size_t)enc->stride * (size_t)enc->out_h;
    const size_t src_luma = (size_t)src_stride * (size_t)h;
    uint8_t *dst_uv = dst + dst_luma;
    const uint8_t *src_uv = src + src_luma;
    int y;

    for (y = 0; y < h; y++)
        memcpy(dst + (size_t)y * enc->stride, src + (size_t)y * src_stride,
               (size_t)w);
    for (y = 0; y < h / 2; y++)
        memcpy(dst_uv + (size_t)y * enc->stride,
               src_uv + (size_t)y * src_stride, (size_t)w);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

elish_encoder *elish_enc_create(elish_codec codec, int width, int height,
                                int fps, int bitrate_bps, const char *es_path)
{
    struct elish_encoder *enc;
    struct v4l2_format cap_fmt, out_fmt;
    struct v4l2_streamparm parm;
    struct v4l2_selection sel;
    struct v4l2_capability qcap;
    const char *env;
    int level;

    if (codec != ELISH_CODEC_H264 && codec != ELISH_CODEC_HEVC) {
        ELISH_ERR("venus: unsupported codec %d", (int)codec);
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        ELISH_ERR("venus: invalid frame size %dx%d", width, height);
        return NULL;
    }
    if ((width & 1) || (height & 1)) {
        ELISH_ERR("venus: NV12 needs even dimensions, got %dx%d", width, height);
        return NULL;
    }
    if (!es_path || !*es_path) {
        ELISH_ERR("venus: no elementary stream path given");
        return NULL;
    }
    if (fps <= 0)
        fps = 30;
    if (fps > ENC_FPS_MAX) {
        ELISH_ERR("venus: fps %d clamped to %d", fps, ENC_FPS_MAX);
        fps = ENC_FPS_MAX;
    }
    if (bitrate_bps <= 0)
        bitrate_bps = 20000000;
    if (bitrate_bps < ENC_BITRATE_MIN) {
        ELISH_ERR("venus: bitrate %d clamped to %d", bitrate_bps, ENC_BITRATE_MIN);
        bitrate_bps = ENC_BITRATE_MIN;
    }
    if (bitrate_bps > ENC_BITRATE_MAX) {
        ELISH_ERR("venus: bitrate %d clamped to %d", bitrate_bps, ENC_BITRATE_MAX);
        bitrate_bps = ENC_BITRATE_MAX;
    }

    enc = calloc(1, sizeof(*enc));
    if (!enc)
        return NULL;

    enc->fd = -1;
    enc->codec = codec;
    enc->codec_fourcc = (codec == ELISH_CODEC_H264) ? V4L2_PIX_FMT_H264
                                                    : V4L2_PIX_FMT_HEVC;
    enc->width = width;
    enc->height = height;
    enc->fps = fps;
    enc->bitrate = bitrate_bps;
    enc->gop = fps;
    env = getenv("ELISH_ENC_GOP");
    if (env && *env) {
        long v = strtol(env, NULL, 10);

        if (v > 0 && v <= ENC_GOP_MAX)
            enc->gop = (int)v;
        else
            ELISH_ERR("venus: ignoring invalid ELISH_ENC_GOP='%s'", env);
    }
    /* Mainline qcom-venus hardcodes the HFI IDR period to 0 ("only the first
     * I-frame is IDR"), so periodic GOP I-frames are not IDRs.  Callers can
     * pass force_keyframe per frame; ELISH_ENC_FORCE_IDR=N makes the encoder
     * request one every N frames without an API change. */
    env = getenv("ELISH_ENC_FORCE_IDR");
    if (env && *env) {
        long v = strtol(env, NULL, 10);

        if (v > 0 && v <= ENC_GOP_MAX)
            enc->force_idr_every = (int)v;
        else
            ELISH_ERR("venus: ignoring invalid ELISH_ENC_FORCE_IDR='%s'", env);
    }

    enc->es_path = strdup(es_path);
    if (!enc->es_path) {
        free(enc);
        return NULL;
    }

    enc->fd = open(enc_devnode(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (enc->fd < 0) {
        ELISH_ERR("venus: open %s: %s", enc_devnode(), strerror(errno));
        goto fail;
    }

    memset(&qcap, 0, sizeof(qcap));
    if (enc_ioctl(enc->fd, VIDIOC_QUERYCAP, &qcap) < 0) {
        ELISH_ERR("venus: QUERYCAP: %s", strerror(errno));
        goto fail;
    }
    if (!(qcap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
        ELISH_ERR("venus: %s is not a multiplanar M2M device (caps %#x)",
                  enc_devnode(), qcap.capabilities);
        goto fail;
    }

    /* Capture (compressed) format first so the driver knows the codec.
     * The driver sizes each capture buffer at ~half a raw frame (9.94 MB at
     * 4224x3136).  Real content stays far below that, but pathological input
     * (uniform random noise) can exceed it even at maximum QP; the firmware
     * then returns empty "not coded" buffers.  ELISH_ENC_CAP_MB overrides the
     * size for such experiments. */
    {
        const char *cap_env = getenv("ELISH_ENC_CAP_MB");
        uint32_t cap_sizeimage = 0;

        if (cap_env && *cap_env) {
            long mb = strtol(cap_env, NULL, 10);

            if (mb > 0 && mb < 512)
                cap_sizeimage = (uint32_t)(mb * 1024 * 1024);
            else
                ELISH_ERR("venus: ignoring invalid ELISH_ENC_CAP_MB='%s'",
                          cap_env);
        }
        if (enc_set_fmt(enc, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                        enc->codec_fourcc, width, height, cap_sizeimage,
                        &cap_fmt) < 0)
            goto fail;
    }
    enc->cap_buf_len = cap_fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    /* Output (raw) format: the driver rounds up to 128x32 alignment. */
    if (enc_set_fmt(enc, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_NV12,
                    width, height, 0, &out_fmt) < 0)
        goto fail;
    enc->out_w = (int)out_fmt.fmt.pix_mp.width;
    enc->out_h = (int)out_fmt.fmt.pix_mp.height;
    enc->stride = (int)out_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (enc->stride <= 0)
        enc->stride = enc->out_w;
    enc->in_frame_size = (size_t)enc->stride * (size_t)enc->out_h * 3 / 2;

    if (enc->out_w < width || enc->out_h < height) {
        ELISH_ERR("venus: driver shrank the frame to %dx%d (asked %dx%d)",
                  enc->out_w, enc->out_h, width, height);
        goto fail;
    }

    /* Ask the firmware to encode exactly width x height out of the padded
     * input buffer.  Without this the bitstream would be 4224x3136. */
    memset(&sel, 0, sizeof(sel));
    sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    sel.target = V4L2_SEL_TGT_CROP;
    sel.r.left = 0;
    sel.r.top = 0;
    sel.r.width = (uint32_t)width;
    sel.r.height = (uint32_t)height;
    if (enc_ioctl(enc->fd, VIDIOC_S_SELECTION, &sel) < 0) {
        ELISH_ERR("venus: S_SELECTION crop %dx%d: %s", width, height,
                  strerror(errno));
        goto fail;
    }
    if ((int)sel.r.width != width || (int)sel.r.height != height) {
        ELISH_ERR("venus: crop readback is %ux%u, wanted %dx%d", sel.r.width,
                  sel.r.height, width, height);
        goto fail;
    }

    /* Nominal frame rate (also drives H.264 VUI timing in the SPS). */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = (uint32_t)fps;
    if (enc_ioctl(enc->fd, VIDIOC_S_PARM, &parm) < 0) {
        ELISH_ERR("venus: S_PARM fps %d: %s", fps, strerror(errno));
        goto fail;
    }

    /* Rate control.  CBR keeps the recording bitrate predictable, which is
     * what the muxed MP4 needs; ELISH_ENC_RC=vbr is available for tests. */
    env = getenv("ELISH_ENC_RC");
    if (enc_s_ctrl(enc, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                   (env && strcasecmp(env, "vbr") == 0)
                       ? V4L2_MPEG_VIDEO_BITRATE_MODE_VBR
                       : V4L2_MPEG_VIDEO_BITRATE_MODE_CBR,
                   "bitrate mode", true) < 0)
        goto fail;
    if (enc_s_ctrl(enc, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate_bps, "bitrate",
                   true) < 0)
        goto fail;
    if (enc_s_ctrl(enc, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK,
                   bitrate_bps * 2, "peak bitrate", false) < 0)
        goto fail;
    if (enc_s_ctrl(enc, V4L2_CID_MPEG_VIDEO_GOP_SIZE, enc->gop, "GOP size",
                   true) < 0)
        goto fail;

    /* Profile/level: the driver defaults to level 1.0 which is nonsense for
     * these sizes, so set the highest level it exposes. */
    if (codec == ELISH_CODEC_H264) {
        if (enc_s_ctrl(enc, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
                       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "H.264 profile",
                       false) < 0)
            goto fail;
        if (enc_s_ctrl(enc, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
                       V4L2_MPEG_VIDEO_H264_LEVEL_5_1, "H.264 level", false) < 0)
            goto fail;
    } else {
        if (enc_s_ctrl(enc, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
                       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, "HEVC profile",
                       false) < 0)
            goto fail;
        level = ((long long)width * height > 8912896)
                    ? V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2
                    : V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1;
        if (enc_s_ctrl(enc, V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, level, "HEVC level",
                       false) < 0)
            goto fail;
    }

    enc->es = fopen(es_path, "wb");
    if (!enc->es) {
        ELISH_ERR("venus: open %s: %s", es_path, strerror(errno));
        goto fail;
    }
    setvbuf(enc->es, NULL, _IOFBF, 1 << 20);

    ELISH_LOG("venus: %s %dx%d (padded %dx%d, stride %d) fps %d bitrate %d "
              "gop %d -> %s",
              codec == ELISH_CODEC_H264 ? "H.264" : "HEVC", width, height,
              enc->out_w, enc->out_h, enc->stride, fps, bitrate_bps, enc->gop,
              es_path);
    return enc;

fail:
    if (enc->es)
        fclose(enc->es);
    if (enc->fd >= 0)
        close(enc->fd);
    free(enc->es_path);
    free(enc);
    return NULL;
}

int elish_enc_start(elish_encoder *enc)
{
    enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    int i;

    if (!enc)
        return -1;
    if (enc->started)
        return 0;

    if (enc_reqbufs(enc, cap_type, ENC_NUM_CAP_BUFS, &enc->cap, &enc->ncap) < 0)
        return -1;
    if (enc_reqbufs(enc, out_type, ENC_NUM_OUT_BUFS, &enc->out, &enc->nout) < 0)
        goto fail_cap;

    enc->free_out = malloc((size_t)enc->nout * sizeof(*enc->free_out));
    if (!enc->free_out) {
        ELISH_ERR("venus: out of memory for the input free list");
        goto fail_out;
    }
    for (i = 0; i < enc->nout; i++) {
        enc->free_out[i] = i;
        /* Deterministic padding: the cropped area is what gets encoded, but a
         * zeroed buffer keeps encoder behaviour repeatable. */
        memset(enc->out[i].addr, 0, enc->out[i].len);
    }
    enc->nfree = enc->nout;

    /* All capture buffers must be queued before the first input frame. */
    for (i = 0; i < enc->ncap; i++) {
        if (enc_qbuf(enc, cap_type, (unsigned int)i, 0, 0) < 0)
            goto fail_out;
    }

    if (enc_ioctl(enc->fd, VIDIOC_STREAMON, &cap_type) < 0) {
        ELISH_ERR("venus: STREAMON capture: %s", strerror(errno));
        goto fail_out;
    }
    enc->streamon_cap = true;

    if (enc_ioctl(enc->fd, VIDIOC_STREAMON, &out_type) < 0) {
        ELISH_ERR("venus: STREAMON output: %s", strerror(errno));
        goto fail_streamoff_cap;
    }
    enc->streamon_out = true;
    enc->started = true;
    return 0;

fail_streamoff_cap:
    (void)enc_ioctl(enc->fd, VIDIOC_STREAMOFF, &cap_type);
    enc->streamon_cap = false;
fail_out:
    free(enc->free_out);
    enc->free_out = NULL;
    enc->nfree = 0;
    enc_free_bufs(enc, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, enc->out, enc->nout);
    enc->out = NULL;
    enc->nout = 0;
fail_cap:
    enc_free_bufs(enc, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, enc->cap, enc->ncap);
    enc->cap = NULL;
    enc->ncap = 0;
    return -1;
}

int elish_enc_push_nv12(elish_encoder *enc, const uint8_t *nv12, int stride,
                        uint64_t pts_ns, bool force_keyframe)
{
    int idx;

    if (!enc || !enc->started) {
        ELISH_ERR("venus: push before start");
        return -1;
    }
    if (!nv12 || stride < enc->width) {
        ELISH_ERR("venus: invalid NV12 frame (stride %d, width %d)", stride,
                  enc->width);
        return -1;
    }

    /* Collect everything the encoder has already finished. */
    if (enc_drain_capture(enc, 0) < 0)
        return -1;

    if (force_keyframe ||
        (enc->force_idr_every > 0 &&
         (enc->pushed % (unsigned long long)enc->force_idr_every) == 0)) {
        struct v4l2_control ctrl = {
            .id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
            .value = 0,
        };

        if (enc_ioctl(enc->fd, VIDIOC_S_CTRL, &ctrl) < 0) {
            if (!enc->warned_force_key) {
                ELISH_ERR("venus: force-keyframe rejected: %s", strerror(errno));
                enc->warned_force_key = true;
            }
        }
    }

    idx = enc_get_free_output(enc);
    if (idx < 0)
        return -1;

    enc_copy_nv12(enc, enc->out[idx].addr, nv12, stride);

    if (enc_qbuf(enc, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, (unsigned int)idx,
                 enc->in_frame_size, pts_ns) < 0)
        return -1;

    enc->pushed++;

    /* Opportunistic drain keeps the ES file in step with the caller. */
    if (enc_drain_capture(enc, 0) < 0)
        return -1;

    return 0;
}

int elish_enc_stop(elish_encoder *enc)
{
    enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    int attempt;
    int ret = 0;

    if (!enc)
        return -1;

    if (enc->started) {
        /* Flush: every queued input frame produces exactly one capture
         * buffer.  B-frames are disabled, so this cannot deadlock. */
        for (attempt = 0; attempt < 200 && enc->encoded < enc->pushed; attempt++) {
            if (enc_drain_capture(enc, 100) < 0)
                break;
        }
        if (enc->encoded != enc->pushed) {
            ELISH_ERR("venus: flush incomplete: %llu of %llu frames encoded",
                      enc->encoded, enc->pushed);
            ret = -1;
        }

        if (enc->streamon_out)
            (void)enc_ioctl(enc->fd, VIDIOC_STREAMOFF, &out_type);
        if (enc->streamon_cap)
            (void)enc_ioctl(enc->fd, VIDIOC_STREAMOFF, &cap_type);
        enc->streamon_out = false;
        enc->streamon_cap = false;

        free(enc->free_out);
        enc->free_out = NULL;
        enc->nfree = 0;
        enc_free_bufs(enc, out_type, enc->out, enc->nout);
        enc->out = NULL;
        enc->nout = 0;
        enc_free_bufs(enc, cap_type, enc->cap, enc->ncap);
        enc->cap = NULL;
        enc->ncap = 0;
        enc->started = false;
    }

    if (enc->es) {
        if (fflush(enc->es) != 0) {
            ELISH_ERR("venus: flush %s: %s", enc->es_path, strerror(errno));
            ret = -1;
        }
        if (fsync(fileno(enc->es)) != 0)
            ELISH_ERR("venus: fsync %s: %s", enc->es_path, strerror(errno));
        if (fclose(enc->es) != 0) {
            ELISH_ERR("venus: close %s: %s", enc->es_path, strerror(errno));
            ret = -1;
        }
        enc->es = NULL;
    }

    return ret;
}

void elish_enc_destroy(elish_encoder *enc)
{
    if (!enc)
        return;

    (void)elish_enc_stop(enc);

    if (enc->fd >= 0)
        close(enc->fd);
    free(enc->es_path);
    free(enc);
}

int elish_mux_mp4(const char *es_path, const char *mp4_path, elish_codec codec)
{
    const char *fmt;
    const char *env;
    char fps_buf[32];
    char setts_buf[128];
    pid_t pid;
    int status;

    if (!es_path || !mp4_path || (codec != ELISH_CODEC_H264 &&
                                  codec != ELISH_CODEC_HEVC)) {
        ELISH_ERR("mux_mp4: invalid arguments");
        return -1;
    }
    fmt = (codec == ELISH_CODEC_H264) ? "h264" : "hevc";

    /* Raw ES has no container timestamps; the raw demuxer needs a hint.
     * H.264 usually carries VUI timing (written from S_PARM) but raw HEVC
     * does not, so a hint keeps the MP4 fps sane. */
    env = getenv("ELISH_MUX_FPS");
    snprintf(fps_buf, sizeof(fps_buf), "%s", (env && *env) ? env : "30");
    /* The raw H.264/HEVC demuxer produces packets without timestamps; the
     * MP4 muxer then invents a broken ~60fps timeline.  setts stamps every
     * packet from a 1/fps time base, which gives an exact CFR MP4. */
    snprintf(setts_buf, sizeof(setts_buf),
             "setts=time_base=1/%s:pts=N:dts=N:duration=1", fps_buf);

    pid = fork();
    if (pid < 0) {
        ELISH_ERR("mux_mp4: fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel",
               "error", "-y", "-framerate", fps_buf, "-f", fmt, "-i", es_path,
               "-c", "copy", "-bsf:v", setts_buf, "-movflags", "+faststart",
               mp4_path, (char *)NULL);
        fprintf(stderr, "elish-camera: mux_mp4: exec ffmpeg failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        ELISH_ERR("mux_mp4: waitpid: %s", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        ELISH_ERR("mux_mp4: ffmpeg failed (status %#x)", status);
        return -1;
    }

    return 0;
}

int elish_enc_get_stats(const elish_encoder *enc, struct elish_enc_stats *out)
{
    if (!enc || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    out->frames_pushed = enc->pushed;
    out->frames_encoded = enc->encoded;
    out->keyframes = enc->keyframes;
    out->skipped = enc->skipped;
    out->bytes = enc->bytes;
    out->width = enc->width;
    out->height = enc->height;
    out->in_width = enc->out_w;
    out->in_height = enc->out_h;
    out->stride = enc->stride;
    out->in_frame_size = enc->in_frame_size;
    out->fps = enc->fps;
    out->bitrate = enc->bitrate;
    out->gop = enc->gop;
    return 0;
}
