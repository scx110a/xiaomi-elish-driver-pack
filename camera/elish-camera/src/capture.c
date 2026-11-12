/*
 * capture.c - CAMSS raw Bayer capture for elish (front OV8856 / rear OV13B10).
 *
 * The media graph is (re)configured from scratch on every open because both
 * sensors share CSID0/VFE0 on the current DTB, so only one camera can stream
 * at a time.  Graph setup uses the well-tested media-ctl CLI; streaming uses
 * plain V4L2 ioctls.
 */
#include "elish_camera.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-controls.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* 相机互斥锁目录：优先 $ELISH_CAMERA_LOCK_DIR，其次 $XDG_RUNTIME_DIR/elish-camera-locks，
 * 最后 /tmp/elish-camera-locks —— 不硬编码任何本机私有路径（便于打包/移植）。 */
static const char *elish_lock_dir(void)
{
    static char buf[256];
    const char *env = getenv("ELISH_CAMERA_LOCK_DIR");
    const char *base = getenv("XDG_RUNTIME_DIR");

    if (env != NULL && env[0] != '\0')
        snprintf(buf, sizeof(buf), "%s", env);
    else if (base != NULL && base[0] != '\0')
        snprintf(buf, sizeof(buf), "%s/elish-camera-locks", base);
    else
        snprintf(buf, sizeof(buf), "/tmp/elish-camera-locks");
    return buf;
}
#define ELISH_MAX_BUFS 8
#define ELISH_MEDIA_DEV "/dev/media0"

struct elish_camera {
    elish_cam_info info;
    int fd_video;
    int fd_sensor;
    int fd_lock;
    void *bufs[ELISH_MAX_BUFS];
    size_t buf_len[ELISH_MAX_BUFS];
    unsigned int nbufs;
    bool streaming;
    int cur_index; /* index of the frame handed out by get_frame */
    int stride;    /* bytes per raw row as reported by the video node */
    bool frame_out;
    char csiphy[32];
    char csid[32];
    char vfe[32];
    char csid_pad[16];
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

uint64_t elish_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

const char *elish_cam_name(elish_cam_id id)
{
    return id == ELISH_CAM_FRONT ? "front" : "rear";
}

const char *elish_bayer_name(elish_bayer bayer)
{
    switch (bayer) {
    case ELISH_BAYER_RGGB: return "RGGB";
    case ELISH_BAYER_GRBG: return "GRBG";
    case ELISH_BAYER_GBRG: return "GBRG";
    case ELISH_BAYER_BGGR: return "BGGR";
    }
    return "?";
}

static int run_cmd_quiet_helper(const char *fmt, ...)
{
    char cmd[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    size_t n = strlen(cmd);
    snprintf(cmd + n, sizeof(cmd) - n, " >/dev/null 2>&1");
    int rc = system(cmd);
    if (rc == -1 || !WIFEXITED(rc))
        return -1;
    return WEXITSTATUS(rc);
}

/* Find first subdevice whose sysfs name starts with prefix, e.g. "ov8856". */
static int find_subdev_by_prefix(const char *prefix, char *out, size_t outlen, char *name_out, size_t name_len)
{
    DIR *d = opendir("/sys/class/video4linux");
    if (!d)
        return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "v4l-subdev", 10) != 0)
            continue;
        char path[512], buf[256];
        snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\n")] = 0;
            if (strncmp(buf, prefix, strlen(prefix)) == 0) {
                snprintf(out, outlen, "/dev/%.24s", de->d_name);
                if (name_out)
                    snprintf(name_out, name_len, "%.63s", buf);
                found = 0;
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static int find_video_by_name(const char *name, char *out, size_t outlen)
{
    DIR *d = opendir("/sys/class/video4linux");
    if (!d)
        return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "video", 5) != 0)
            continue;
        char path[512], buf[256];
        snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\n")] = 0;
            if (strcmp(buf, name) == 0) {
                snprintf(out, outlen, "/dev/%.24s", de->d_name);
                found = 0;
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static int acquire_lock(const char *name, int *fd_out)
{
    char path[512];
    const char *lock_dir = elish_lock_dir();

    snprintf(path, sizeof(path), "%s/%s", lock_dir, name);
    mkdir(lock_dir, 0775);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    if (fd < 0) {
        ELISH_ERR("cannot open lock %s: %s", path, strerror(errno));
        return -1;
    }
    /* bounded wait: another elish-camera instance may still be streaming */
    int tries = 0;
    while (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK && errno != EINTR) {
            ELISH_ERR("flock %s failed: %s", path, strerror(errno));
            close(fd);
            return -1;
        }
        if (++tries > 50) {
            ELISH_ERR("camera busy: another process holds %s", path);
            close(fd);
            return -1;
        }
        usleep(200 * 1000);
    }
    *fd_out = fd;
    return 0;
}

/* media bus code -> media-ctl name and V4L2 packed pixfmt */
struct mbus_map {
    unsigned int code;
    const char *mbus_name;
    unsigned int pixfmt;
};

static const struct mbus_map mbus_maps[] = {
    { MEDIA_BUS_FMT_SBGGR10_1X10, "SBGGR10_1X10", V4L2_PIX_FMT_SBGGR10P },
    { MEDIA_BUS_FMT_SGRBG10_1X10, "SGRBG10_1X10", V4L2_PIX_FMT_SGRBG10P },
    { MEDIA_BUS_FMT_SGBRG10_1X10, "SGBRG10_1X10", V4L2_PIX_FMT_SGBRG10P },
    { MEDIA_BUS_FMT_SRGGB10_1X10, "SRGGB10_1X10", V4L2_PIX_FMT_SRGGB10P },
    { MEDIA_BUS_FMT_SBGGR8_1X8,   "SBGGR8_1X8",   V4L2_PIX_FMT_SBGGR8 },
    { MEDIA_BUS_FMT_SGRBG8_1X8,   "SGRBG8_1X8",   V4L2_PIX_FMT_SGRBG8 },
    { MEDIA_BUS_FMT_SGBRG8_1X8,   "SGBRG8_1X8",   V4L2_PIX_FMT_SGBRG8 },
    { MEDIA_BUS_FMT_SRGGB8_1X8,   "SRGGB8_1X8",   V4L2_PIX_FMT_SRGGB8 },
};

static const struct mbus_map *mbus_lookup_by_code(unsigned int code)
{
    for (size_t i = 0; i < sizeof(mbus_maps) / sizeof(mbus_maps[0]); i++)
        if (mbus_maps[i].code == code)
            return &mbus_maps[i];
    return NULL;
}

static int subdev_g_fmt_code(const char *dev, unsigned int *code, int *w, int *h)
{
    struct v4l2_subdev_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.pad = 0;
    fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    int fd = open(dev, O_RDWR);
    if (fd < 0)
        return -1;
    int rc = ioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt);
    close(fd);
    if (rc < 0)
        return -1;
    *code = fmt.format.code;
    *w = fmt.format.width;
    *h = fmt.format.height;
    return 0;
}

static int subdev_query_ctrl(const char *dev, unsigned int id, int *min, int *max, int *def)
{
    struct v4l2_queryctrl q;
    memset(&q, 0, sizeof(q));
    q.id = id;
    int fd = open(dev, O_RDWR);
    if (fd < 0)
        return -1;
    int rc = ioctl(fd, VIDIOC_QUERYCTRL, &q);
    close(fd);
    if (rc < 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED))
        return -1;
    *min = q.minimum;
    *max = q.maximum;
    *def = q.default_value;
    return 0;
}

static int subdev_s_ctrl(const char *dev, unsigned int id, int value)
{
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = value;
    int fd = open(dev, O_RDWR);
    if (fd < 0)
        return -1;
    int rc = ioctl(fd, VIDIOC_S_CTRL, &ctrl);
    close(fd);
    return rc < 0 ? -errno : 0;
}

/*
 * Request a sensor mode.  Sensors pick the mode by width/height and force their
 * own mbus code, so we send the mode we want and read the code back.
 */
static int configure_media(elish_camera *cam, int width, int height)
{
    const elish_cam_id id = cam->info.id;
    char sensor_dev[64], sensor_name[64];
    const char *sensor_prefix = id == ELISH_CAM_FRONT ? "ov8856" : "ov13b10";

    if (find_subdev_by_prefix(sensor_prefix, sensor_dev, sizeof(sensor_dev),
                              sensor_name, sizeof(sensor_name)) < 0) {
        ELISH_ERR("sensor subdev %s not found", sensor_prefix);
        return -1;
    }
    snprintf(cam->info.sensor_subdev, sizeof(cam->info.sensor_subdev), "%s", sensor_dev);

    /* Clear any stale graph (a leftover stream makes media-ctl reset fail). */
    int rc = -1;
    for (int i = 0; i < 5; i++) {
        rc = run_cmd_quiet_helper("media-ctl -d %s -r", ELISH_MEDIA_DEV);
        if (rc == 0)
            break;
        usleep(400 * 1000);
    }
    if (rc != 0) {
        ELISH_ERR("media-ctl -r on %s failed (busy?); check 'fuser -v %s'",
                  ELISH_MEDIA_DEV, ELISH_MEDIA_DEV);
        return -1;
    }

    /* Enable the two dynamic links of the wanted pipeline. */
    if (id == ELISH_CAM_FRONT) {
        snprintf(cam->csiphy, sizeof(cam->csiphy), "msm_csiphy4");
        snprintf(cam->csid, sizeof(cam->csid), "msm_csid0");
        snprintf(cam->vfe, sizeof(cam->vfe), "msm_vfe0_rdi0");
        snprintf(cam->csid_pad, sizeof(cam->csid_pad), "1");
    } else {
        snprintf(cam->csiphy, sizeof(cam->csiphy), "msm_csiphy1");
        snprintf(cam->csid, sizeof(cam->csid), "msm_csid0");
        snprintf(cam->vfe, sizeof(cam->vfe), "msm_vfe0_rdi1");
        snprintf(cam->csid_pad, sizeof(cam->csid_pad), "2");
    }
    if (run_cmd_quiet_helper("media-ctl -d %s -l '\"%s\":1 -> \"%s\":0[1]'",
                             ELISH_MEDIA_DEV, cam->csiphy, cam->csid) != 0) {
        ELISH_ERR("enabling %s:1 -> %s:0 failed", cam->csiphy, cam->csid);
        return -1;
    }
    if (run_cmd_quiet_helper("media-ctl -d %s -l '\"%s\":%s -> \"%s\":0[1]'",
                             ELISH_MEDIA_DEV, cam->csid, cam->csid_pad, cam->vfe) != 0) {
        ELISH_ERR("enabling %s:%s -> %s:0 failed", cam->csid, cam->csid_pad, cam->vfe);
        return -1;
    }

    /* Ask the sensor for the mode; the driver forces its own mbus code. */
    const char *try_codes[] = { "SBGGR10_1X10", "SGRBG10_1X10" };
    int ok = 0;
    for (size_t i = 0; i < 2 && !ok; i++) {
        rc = run_cmd_quiet_helper("media-ctl -d %s -V '\"%s\":0 [fmt:%s/%dx%d]'",
                                  ELISH_MEDIA_DEV, sensor_name, try_codes[i], width, height);
        if (rc == 0)
            ok = 1;
    }
    if (!ok) {
        ELISH_ERR("setting sensor %s mode %dx%d failed", sensor_name, width, height);
        return -1;
    }

    unsigned int code = 0;
    int got_w = 0, got_h = 0;
    if (subdev_g_fmt_code(sensor_dev, &code, &got_w, &got_h) < 0) {
        ELISH_ERR("reading sensor format failed");
        return -1;
    }
    const struct mbus_map *map = mbus_lookup_by_code(code);
    if (!map) {
        ELISH_ERR("unsupported sensor mbus code 0x%04x", code);
        return -1;
    }
    if (got_w != width || got_h != height) {
        ELISH_LOG("sensor mode adjusted: requested %dx%d, got %dx%d",
                  width, height, got_w, got_h);
    }
    cam->info.width = got_w;
    cam->info.height = got_h;

    /* Propagate the format through the pipeline. */
    const char *entities[][2] = {
        { cam->csiphy, "0" }, { cam->csiphy, "1" },
        { cam->csid, "0" },   { cam->csid, cam->csid_pad },
        { cam->vfe, "0" },    { cam->vfe, "1" },
    };
    for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
        if (run_cmd_quiet_helper("media-ctl -d %s -V '\"%s\":%s [fmt:%s/%dx%d]'",
                                 ELISH_MEDIA_DEV, entities[i][0], entities[i][1],
                                 map->mbus_name, got_w, got_h) != 0) {
            ELISH_ERR("setting format on %s:%s failed", entities[i][0], entities[i][1]);
            return -1;
        }
    }

    /* Find the video node feeding this VFE RDI ("msm_vfe0_rdi1" -> "msm_vfe0_video1"). */
    char video_name[64];
    snprintf(video_name, sizeof(video_name), "%s", cam->vfe);
    char *rdi = strstr(video_name, "_rdi");
    if (!rdi) {
        ELISH_ERR("unexpected VFE entity name %s", cam->vfe);
        return -1;
    }
    const char rdi_index = rdi[4];
    snprintf(rdi, sizeof(video_name) - (size_t)(rdi - video_name), "_video%c", rdi_index);
    if (find_video_by_name(video_name, cam->info.video_dev, sizeof(cam->info.video_dev)) < 0) {
        ELISH_ERR("video node %s not found", video_name);
        return -1;
    }

    /* Actual colour filter order, measured on hardware from the two green
     * channels' means (see docs/architecture.md §2): front OV8856 reads out
     * BGGR, rear OV13B10 reads out GRBG.  The driver-reported mbus code
     * only describes the CSI routing format, not the physical CFA order. */
    cam->info.bayer = (id == ELISH_CAM_FRONT) ? ELISH_BAYER_BGGR : ELISH_BAYER_GRBG;
    ELISH_LOG("configured %s: %dx%d bayer=%s mbus=0x%04x video=%s sensor=%s",
              elish_cam_name(id), got_w, got_h, elish_bayer_name(cam->info.bayer),
              code, cam->info.video_dev, sensor_dev);
    return 0;
}

static int video_configure(elish_camera *cam, unsigned int pixfmt)
{
    int fd = open(cam->info.video_dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ELISH_ERR("open %s: %s", cam->info.video_dev, strerror(errno));
        return -1;
    }
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = cam->info.width;
    fmt.fmt.pix_mp.height = cam->info.height;
    fmt.fmt.pix_mp.pixelformat = pixfmt;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        ELISH_ERR("VIDIOC_S_FMT %dx%d pixfmt=0x%08x: %s", cam->info.width,
                  cam->info.height, pixfmt, strerror(errno));
        close(fd);
        return -1;
    }
    cam->info.width = fmt.fmt.pix_mp.width;
    cam->info.height = fmt.fmt.pix_mp.height;
    cam->stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    ELISH_LOG("video format: %dx%d pixfmt=0x%08x stride=%u size=%u", cam->info.width,
              cam->info.height, fmt.fmt.pix_mp.pixelformat, cam->stride,
              fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = ELISH_MAX_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        ELISH_ERR("VIDIOC_REQBUFS: %s", strerror(errno));
        close(fd);
        return -1;
    }
    cam->nbufs = req.count;
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ELISH_ERR("VIDIOC_QUERYBUF[%u]: %s", i, strerror(errno));
            close(fd);
            return -1;
        }
        cam->buf_len[i] = planes[0].length;
        cam->bufs[i] = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                            fd, planes[0].m.mem_offset);
        if (cam->bufs[i] == MAP_FAILED) {
            ELISH_ERR("mmap[%u]: %s", i, strerror(errno));
            close(fd);
            return -1;
        }
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            ELISH_ERR("VIDIOC_QBUF[%u]: %s", i, strerror(errno));
            close(fd);
            return -1;
        }
    }
    cam->fd_video = fd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

static int camera_finish_open(elish_camera *cam);

elish_camera *elish_cam_open(elish_cam_id id, int width, int height, int fps)
{
    elish_camera *cam = calloc(1, sizeof(*cam));
    if (!cam)
        return NULL;
    cam->fd_video = -1;
    cam->fd_sensor = -1;
    cam->fd_lock = -1;
    cam->cur_index = -1;
    cam->info.id = id;
    cam->info.fps = fps > 0 ? fps : 30;

    if (acquire_lock("camera.lock", &cam->fd_lock) < 0) {
        free(cam);
        return NULL;
    }

    if (id == ELISH_CAM_FRONT) {
        if (width <= 0 || height <= 0) {
            width = 3280;
            height = 2464;
        }
    } else {
        if (width <= 0 || height <= 0) {
            width = 4208;
            height = 3120;
        }
    }

    if (configure_media(cam, width, height) < 0) {
        elish_cam_close(cam);
        return NULL;
    }
    if (camera_finish_open(cam) < 0) {
        elish_cam_close(cam);
        return NULL;
    }
    return cam;
}

/* Open sensor subdev + video node, set controls and query ranges. */
static int camera_finish_open(elish_camera *cam)
{
    /* Sensor controls: never enable vertical flip on the front sensor, it
     * makes the 3280x2464 mode produce no frames (see architecture.md). */
    cam->fd_sensor = open(cam->info.sensor_subdev, O_RDWR | O_CLOEXEC);
    if (cam->fd_sensor < 0) {
        ELISH_ERR("open sensor %s: %s", cam->info.sensor_subdev, strerror(errno));
        return -1;
    }
    subdev_s_ctrl(cam->info.sensor_subdev, V4L2_CID_VFLIP, 0);
    subdev_s_ctrl(cam->info.sensor_subdev, V4L2_CID_HFLIP, 0);

    /* Map the sensor mbus code to the V4L2 packed RAW10 pixel format. */
    unsigned int code = 0;
    int w = 0, h = 0;
    if (subdev_g_fmt_code(cam->info.sensor_subdev, &code, &w, &h) < 0) {
        ELISH_ERR("cannot read sensor format");
        return -1;
    }
    const struct mbus_map *map = mbus_lookup_by_code(code);
    if (!map) {
        ELISH_ERR("unsupported mbus code 0x%04x", code);
        return -1;
    }
    if (video_configure(cam, map->pixfmt) < 0)
        return -1;

    /* Query control ranges for AE. */
    int def = 0;
    if (subdev_query_ctrl(cam->info.sensor_subdev, V4L2_CID_EXPOSURE,
                          &cam->info.exposure_min, &cam->info.exposure_max, &def) == 0) {
        cam->info.exposure_min = cam->info.exposure_min > 6 ? cam->info.exposure_min : 6;
    } else {
        cam->info.exposure_min = 6;
        cam->info.exposure_max = 2400;
    }
    if (subdev_query_ctrl(cam->info.sensor_subdev, V4L2_CID_ANALOGUE_GAIN,
                          &cam->info.analogue_gain_min, &cam->info.analogue_gain_max, &def) < 0) {
        cam->info.analogue_gain_min = 128;
        cam->info.analogue_gain_max = 2047;
    }
    if (subdev_query_ctrl(cam->info.sensor_subdev, V4L2_CID_DIGITAL_GAIN,
                          &cam->info.digital_gain_min, &cam->info.digital_gain_max, &def) < 0) {
        cam->info.digital_gain_min = 1024;
        cam->info.digital_gain_max = 4095;
    }
    ELISH_LOG("%s ctrl ranges: exposure %d..%d gain %d..%d dgain %d..%d",
              elish_cam_name(cam->info.id), cam->info.exposure_min, cam->info.exposure_max,
              cam->info.analogue_gain_min, cam->info.analogue_gain_max,
              cam->info.digital_gain_min, cam->info.digital_gain_max);

    /* Sensible default exposure/gain so the first frame is usable. */
    if (cam->info.id == ELISH_CAM_FRONT)
        elish_cam_set_exposure(cam, 1508, 1536, 2048);
    else
        elish_cam_set_exposure(cam, 1590, 1984, 4095);
    return 0;
}

const elish_cam_info *elish_cam_info_get(elish_camera *cam)
{
    return &cam->info;
}

int elish_cam_start(elish_camera *cam)
{
    if (cam->streaming)
        return 0;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(cam->fd_video, VIDIOC_STREAMON, &type) < 0) {
        ELISH_ERR("VIDIOC_STREAMON: %s", strerror(errno));
        return -1;
    }
    cam->streaming = true;
    return 0;
}

int elish_cam_get_frame(elish_camera *cam, elish_raw_image *img, uint64_t *ts_ns)
{
    if (!cam->streaming)
        return -1;
    if (cam->frame_out) {
        ELISH_ERR("get_frame called twice without put_frame");
        return -1;
    }
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;
    /* Bound the wait so a stalled sensor/media graph cannot hang the caller
     * forever (this happened intermittently after media-graph reconfigs). */
    struct pollfd pfd = { .fd = cam->fd_video, .events = POLLIN };
    int pr = poll(&pfd, 1, 2000);
    if (pr <= 0) {
        ELISH_ERR("VIDIOC_DQBUF: no frame within 2s%s", pr < 0 ? strerror(errno) : "");
        return -1;
    }
    if (ioctl(cam->fd_video, VIDIOC_DQBUF, &buf) < 0) {
        ELISH_ERR("VIDIOC_DQBUF: %s", strerror(errno));
        return -1;
    }
    cam->cur_index = buf.index;
    cam->frame_out = true;
    img->data = cam->bufs[buf.index];
    img->width = cam->info.width;
    img->height = cam->info.height;
    img->stride = cam->stride;
    if (ts_ns) {
        *ts_ns = (uint64_t)buf.timestamp.tv_sec * 1000000000ull +
                 (uint64_t)buf.timestamp.tv_usec * 1000ull;
    }
    return 0;
}

int elish_cam_put_frame(elish_camera *cam)
{
    if (!cam->frame_out)
        return -1;
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = cam->cur_index;
    buf.m.planes = planes;
    buf.length = 1;
    int rc = ioctl(cam->fd_video, VIDIOC_QBUF, &buf);
    cam->frame_out = false;
    if (rc < 0) {
        ELISH_ERR("VIDIOC_QBUF: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int elish_cam_set_exposure(elish_camera *cam, int exposure, int analogue_gain, int digital_gain)
{
    int rc = 0;
    if (exposure >= 0)
        rc |= subdev_s_ctrl(cam->info.sensor_subdev, V4L2_CID_EXPOSURE, exposure);
    if (analogue_gain >= 0)
        rc |= subdev_s_ctrl(cam->info.sensor_subdev, V4L2_CID_ANALOGUE_GAIN, analogue_gain);
    if (digital_gain >= 0)
        rc |= subdev_s_ctrl(cam->info.sensor_subdev, V4L2_CID_DIGITAL_GAIN, digital_gain);
    return rc < 0 ? -1 : 0;
}

int elish_cam_set_focus(elish_camera *cam, int position)
{
    if (cam->info.id != ELISH_CAM_REAR)
        return -1;
    if (position < 0)
        position = 0;
    if (position > 1023)
        position = 1023;
    const char *paths[] = {
        "/sys/bus/i2c/devices/20-000c/focus",
        "/sys/bus/i2c/devices/23-000c/focus",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = open(paths[i], O_WRONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d", position);
        ssize_t wr = write(fd, buf, n);
        close(fd);
        if (wr == n)
            return 0;
    }
    ELISH_ERR("cannot write VCM focus sysfs");
    return -1;
}

void elish_cam_stop(elish_camera *cam)
{
    if (!cam || !cam->streaming)
        return;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(cam->fd_video, VIDIOC_STREAMOFF, &type);
    cam->streaming = false;
    cam->frame_out = false;
}

void elish_cam_close(elish_camera *cam)
{
    if (!cam)
        return;
    if (cam->streaming)
        elish_cam_stop(cam);
    if (cam->fd_video >= 0) {
        for (unsigned int i = 0; i < cam->nbufs; i++) {
            if (cam->bufs[i] && cam->bufs[i] != MAP_FAILED)
                munmap(cam->bufs[i], cam->buf_len[i]);
        }
        close(cam->fd_video);
    }
    if (cam->fd_sensor >= 0)
        close(cam->fd_sensor);
    if (cam->fd_lock >= 0) {
        flock(cam->fd_lock, LOCK_UN);
        close(cam->fd_lock);
    }
    free(cam);
}
