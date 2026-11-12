/*
 * loopback.c - standard V4L2 virtual camera output (v4l2loopback producer).
 *
 * The producer (this file) uses the V4L2 OUTPUT queue of a v4l2loopback
 * device: VIDIOC_S_FMT(OUTPUT) selects the format that CAPTURE consumers
 * see, frames are copied into mmap'ed OUTPUT buffers and queued with
 * VIDIOC_QBUF.  The kernel v4l2loopback driver immediately publishes each
 * queued buffer to all CAPTURE readers, so one producer can feed several
 * consumers at once (module parameter max_openers=10 on this machine).
 *
 * v4l2loopback specifics that shaped this implementation (0.12.7 source):
 *  - NV12 is registered as a PLANAR format: bytesperline == width and
 *    sizeimage == width * height * 3 / 2, i.e. no stride padding.
 *  - The OUTPUT queue has no concept of "free" buffers: VIDIOC_QBUF copies
 *    metadata only (frame data is already in the mmap'ed image) and marks
 *    the buffer as the newest frame.  VIDIOC_DQBUF on an OUTPUT queue
 *    returns the *last queued* buffer, so the standard producer loop is
 *    "fill buffer, QBUF, repeat" with a small round-robin ring, which is
 *    what we do (each buffer is re-queued in turn).
 *  - G_FMT(CAPTURE) / S_FMT(CAPTURE) return -EINVAL / -EBUSY until the
 *    producer has set the format and started streaming, therefore the
 *    producer must be started before consumers open the device.
 *  - S_PARM(OUTPUT) is supported and sets the time-per-frame that
 *    consumers read back with G_PARM, so we propagate fps there.
 *
 * The public API is fixed by include/elish_camera.h: elish_lb_open(),
 * elish_lb_push_nv12(), elish_lb_close().
 */
#include "elish_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

/* Keep the ring small: every buffer is a full-resolution vmalloc'ed image. */
#define LB_MAX_BUFS 8
#define LB_NUM_BUFS 4

struct lb_buf {
    void *start;
    size_t length;
};

struct elish_loopback {
    int fd;
    int width;
    int height;
    int fps;

    uint32_t dev_stride;   /* bytesperline reported for the OUTPUT format */
    size_t payload;        /* NV12 image size expected by the driver       */
    size_t buffer_length;  /* mmap'ed length of one driver buffer          */
    unsigned nbufs;
    struct lb_buf bufs[LB_MAX_BUFS];
    unsigned next_buf;

    uint64_t interval_ns;
    uint64_t deadline_ns;
    uint64_t frames_pushed;
    uint64_t frames_dropped;
    bool streaming;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static uint64_t lb_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Retry ioctls interrupted by a signal; everything else is returned as-is. */
static int lb_ioctl(int fd, unsigned long req, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static int lb_ioctl_retry_eagain(int fd, unsigned long req, void *arg)
{
    unsigned tries = 0;
    int ret;

    for (;;) {
        ret = lb_ioctl(fd, req, arg);
        if (ret == 0)
            return 0;
        if (errno != EAGAIN && errno != EBUSY && errno != EINTR)
            return -1;
        if (++tries >= 1000) { /* ~1 s worst case */
            errno = ETIMEDOUT;
            return -1;
        }
        struct timespec ts = { 0, 1000000 }; /* 1 ms */
        nanosleep(&ts, NULL);
    }
}

static unsigned lb_clamp_bufs(unsigned want)
{
    if (want < 1)
        want = 1;
    if (want > LB_MAX_BUFS)
        want = LB_MAX_BUFS;
    return want;
}

/* Sleep until an absolute CLOCK_MONOTONIC deadline.  Returns early with
 * -1/EINTR is not treated as error: the caller re-checks the clock. */
static void lb_sleep_until(uint64_t deadline_ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

/* ------------------------------------------------------------------ */
/* open / configure / close                                           */
/* ------------------------------------------------------------------ */

elish_loopback *elish_lb_open(const char *dev, int width, int height, int fps)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_streamparm parm;
    struct v4l2_requestbuffers req;
    elish_loopback *lb;
    unsigned i;

    if (!dev || !*dev) {
        ELISH_ERR("elish_lb_open: no device given");
        return NULL;
    }
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536) {
        ELISH_ERR("elish_lb_open: invalid size %dx%d", width, height);
        return NULL;
    }
    if (fps <= 0)
        fps = 30;
    if (fps > 1000)
        fps = 1000;

    lb = calloc(1, sizeof(*lb));
    if (!lb) {
        ELISH_ERR("elish_lb_open: out of memory");
        return NULL;
    }
    lb->fd = -1;
    lb->width = width;
    lb->height = height;
    lb->fps = fps;
    lb->next_buf = 0;

    lb->fd = open(dev, O_RDWR | O_CLOEXEC);
    if (lb->fd < 0) {
        ELISH_ERR("elish_lb_open: cannot open %s: %s", dev, strerror(errno));
        goto fail;
    }

    memset(&cap, 0, sizeof(cap));
    if (lb_ioctl(lb->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        ELISH_ERR("elish_lb_open: %s is not a V4L2 device: %s", dev, strerror(errno));
        goto fail;
    }
    {
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                 : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_OUTPUT) || !(caps & V4L2_CAP_STREAMING)) {
            ELISH_ERR("elish_lb_open: %s lacks V4L2 OUTPUT/STREAMING (caps=0x%08x); "
                      "is it really a v4l2loopback device?",
                      dev, caps);
            goto fail;
        }
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
            ELISH_ERR("elish_lb_open: warning: %s does not announce CAPTURE "
                      "(exclusive_caps=1?); consumers will not see it", dev);
        }
    }

    /* Select the format consumers will read.  v4l2loopback honours this
     * only while no writer has started yet (ready_for_capture == 0). */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = (uint32_t)width;
    fmt.fmt.pix.height = (uint32_t)height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    fmt.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_601;
    fmt.fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    fmt.fmt.pix.bytesperline = (uint32_t)width;
    fmt.fmt.pix.sizeimage = (uint32_t)((uint64_t)width * height * 3 / 2);

    if (lb_ioctl(lb->fd, VIDIOC_S_FMT, &fmt) < 0) {
        ELISH_ERR("elish_lb_open: VIDIOC_S_FMT NV12 %dx%d failed: %s",
                  width, height, strerror(errno));
        goto fail;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
        (int)fmt.fmt.pix.width != width || (int)fmt.fmt.pix.height != height) {
        ELISH_ERR("elish_lb_open: driver did not accept NV12 %dx%d "
                  "(got fourcc=0x%08x %ux%u).  If the size was clamped, the "
                  "v4l2loopback module needs bigger max_width/max_height "
                  "(current max: 8192x8192) or the device already has an "
                  "active producer.  Do not rmmod without asking the Lead.",
                  width, height, fmt.fmt.pix.pixelformat,
                  fmt.fmt.pix.width, fmt.fmt.pix.height);
        goto fail;
    }

    /* Re-read the final format; v4l2loopback reports the page-aligned
     * sizeimage in S_FMT but keeps the true payload in pix_format. */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (lb_ioctl(lb->fd, VIDIOC_G_FMT, &fmt) < 0) {
        ELISH_ERR("elish_lb_open: VIDIOC_G_FMT failed: %s", strerror(errno));
        goto fail;
    }
    lb->dev_stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline
                                              : (uint32_t)width;
    lb->payload = (size_t)lb->dev_stride * (size_t)height * 3 / 2;
    if (lb->dev_stride < (uint32_t)width) {
        ELISH_ERR("elish_lb_open: driver bytesperline %u < width %d",
                  lb->dev_stride, width);
        goto fail;
    }

    /* Tell consumers the nominal frame rate (G_PARM / ffmpeg / gst). */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = (uint32_t)fps;
    if (lb_ioctl(lb->fd, VIDIOC_S_PARM, &parm) < 0) {
        ELISH_LOG("elish_lb_open: VIDIOC_S_PARM(%d fps) failed: %s "
                  "(ignored, pacing is done in userspace)",
                  fps, strerror(errno));
    }

    memset(&req, 0, sizeof(req));
    req.count = LB_NUM_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_MMAP;
    if (lb_ioctl(lb->fd, VIDIOC_REQBUFS, &req) < 0) {
        ELISH_ERR("elish_lb_open: VIDIOC_REQBUFS(%u) failed: %s",
                  LB_NUM_BUFS, strerror(errno));
        goto fail;
    }
    lb->nbufs = lb_clamp_bufs(req.count);
    if (lb->nbufs == 0) {
        ELISH_ERR("elish_lb_open: driver returned 0 OUTPUT buffers");
        goto fail;
    }
    if (lb->nbufs < LB_NUM_BUFS)
        ELISH_LOG("elish_lb_open: only %u mmap buffers available (asked %u), "
                  "continuing", lb->nbufs, LB_NUM_BUFS);

    for (i = 0; i < lb->nbufs; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (lb_ioctl(lb->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ELISH_ERR("elish_lb_open: VIDIOC_QUERYBUF(%u) failed: %s",
                      i, strerror(errno));
            goto fail;
        }
        lb->buffer_length = buf.length;
        lb->bufs[i].length = buf.length;
        lb->bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, lb->fd, (off_t)buf.m.offset);
        if (lb->bufs[i].start == MAP_FAILED) {
            lb->bufs[i].start = NULL;
            ELISH_ERR("elish_lb_open: mmap buffer %u (%zu bytes) failed: %s",
                      i, (size_t)buf.length, strerror(errno));
            goto fail;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (lb_ioctl(lb->fd, VIDIOC_STREAMON, &type) < 0) {
        ELISH_ERR("elish_lb_open: VIDIOC_STREAMON(OUTPUT) failed: %s",
                  strerror(errno));
        goto fail;
    }
    lb->streaming = true;

    /* Prime the queue so that CAPTURE readers get a frame immediately even
     * before the first real push.  Zero-filled NV12 (black) is fine. */
    for (i = 0; i < lb->nbufs; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.bytesused = (uint32_t)lb->payload;
        if (lb_ioctl_retry_eagain(lb->fd, VIDIOC_QBUF, &buf) < 0) {
            ELISH_ERR("elish_lb_open: priming VIDIOC_QBUF(%u) failed: %s",
                      i, strerror(errno));
            goto fail;
        }
    }

    lb->interval_ns = 1000000000ull / (uint64_t)fps;
    if (lb->interval_ns == 0)
        lb->interval_ns = 1;
    lb->deadline_ns = lb_now_ns();

    ELISH_LOG("loopback producer ready: %s NV12 %dx%d @%dfps, %u buffers, "
              "stride=%u payload=%zu; consumers see this format",
              dev, width, height, fps, lb->nbufs, lb->dev_stride, lb->payload);
    return lb;

fail:
    elish_lb_close(lb);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* frame push                                                         */
/* ------------------------------------------------------------------ */

/* Copy the caller's NV12 (Y stride = stride, UV plane at stride*height,
 * UV stride = stride) into a driver buffer whose stride is dev_stride. */
static void lb_copy_nv12(elish_loopback *lb, void *dst, const uint8_t *src,
                         int stride)
{
    const size_t ysize = (size_t)lb->width * (size_t)lb->height;
    const size_t uvsize = ysize / 2;
    const void *uv_src = src + (size_t)stride * (size_t)lb->height;
    void *uv_dst = (uint8_t *)dst + (size_t)lb->dev_stride * (size_t)lb->height;

    if ((uint32_t)stride == lb->dev_stride) {
        memcpy(dst, src, ysize + uvsize);
        return;
    }

    /* Row-by-row: caller stride != driver stride (e.g. 64-byte aligned). */
    for (int y = 0; y < lb->height; y++)
        memcpy((uint8_t *)dst + (size_t)y * lb->dev_stride,
               src + (size_t)y * stride, (size_t)lb->width);
    for (int y = 0; y < lb->height / 2; y++)
        memcpy((uint8_t *)uv_dst + (size_t)y * lb->dev_stride,
               (const uint8_t *)uv_src + (size_t)y * stride, (size_t)lb->width);
}

int elish_lb_push_nv12(elish_loopback *lb, const uint8_t *nv12, int stride)
{
    struct v4l2_buffer buf;
    uint64_t now;

    if (!lb || lb->fd < 0 || !lb->streaming) {
        ELISH_ERR("elish_lb_push_nv12: loopback not open");
        return -1;
    }
    if (!nv12) {
        ELISH_ERR("elish_lb_push_nv12: NULL frame");
        return -1;
    }
    if (stride < lb->width) {
        ELISH_ERR("elish_lb_push_nv12: stride %d < width %d", stride, lb->width);
        return -1;
    }

    /* Pace to the requested fps.  If we fell more than one frame behind
     * (slow CPU, scheduling hiccup), resynchronise instead of trying to
     * catch up with a burst -- that keeps long runs stable. */
    now = lb_now_ns();
    if (now < lb->deadline_ns) {
        lb_sleep_until(lb->deadline_ns);
        now = lb_now_ns();
    } else if (now > lb->deadline_ns + lb->interval_ns) {
        uint64_t late = now - lb->deadline_ns;
        lb->frames_dropped += late / lb->interval_ns;
        lb->deadline_ns = now;
    }

    {
        struct lb_buf *b = &lb->bufs[lb->next_buf];
        lb_copy_nv12(lb, b->start, nv12, stride);

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = lb->next_buf;
        buf.bytesused = (uint32_t)lb->payload;
        buf.field = V4L2_FIELD_NONE;
        if (lb_ioctl_retry_eagain(lb->fd, VIDIOC_QBUF, &buf) < 0) {
            ELISH_ERR("elish_lb_push_nv12: VIDIOC_QBUF(%u) failed: %s",
                      lb->next_buf, strerror(errno));
            if (errno == EINVAL || errno == ENODEV || errno == ENOTTY)
                return -1;
            /* transient: count it and keep going */
            lb->frames_dropped++;
        } else {
            lb->frames_pushed++;
        }
        lb->next_buf = (lb->next_buf + 1) % lb->nbufs;
    }

    lb->deadline_ns += lb->interval_ns;
    if (lb->deadline_ns < now) /* deadline slipped behind: resync */
        lb->deadline_ns = now + lb->interval_ns;

    return 0;
}

/* ------------------------------------------------------------------ */
/* close                                                              */
/* ------------------------------------------------------------------ */

void elish_lb_close(elish_loopback *lb)
{
    unsigned i;

    if (!lb)
        return;

    if (lb->fd >= 0 && lb->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (lb_ioctl(lb->fd, VIDIOC_STREAMOFF, &type) < 0)
            ELISH_LOG("elish_lb_close: VIDIOC_STREAMOFF failed: %s",
                      strerror(errno));
        lb->streaming = false;
    }

    for (i = 0; i < lb->nbufs; i++) {
        if (lb->bufs[i].start && lb->bufs[i].length)
            munmap(lb->bufs[i].start, lb->bufs[i].length);
        lb->bufs[i].start = NULL;
        lb->bufs[i].length = 0;
    }

    if (lb->fd >= 0) {
        close(lb->fd);
        lb->fd = -1;
    }

    if (lb->frames_pushed || lb->frames_dropped)
        ELISH_LOG("loopback closed: %llu frames pushed, %llu pacing drops",
                  (unsigned long long)lb->frames_pushed,
                  (unsigned long long)lb->frames_dropped);

    free(lb);
}
