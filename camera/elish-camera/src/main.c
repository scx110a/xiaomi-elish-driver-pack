/*
 * main.c - elish-camera command line front-end.
 *
 *   elish-camera list
 *   elish-camera photo  --camera front|rear -o out.png [options]
 *   elish-camera record --camera front|rear -o out.mp4 -d seconds [options]
 *   elish-camera stream --camera front|rear --loopback /dev/video2 [options]
 *
 * Pipeline: CAMSS packed RAW10 -> GPU debayer (NV12/RGB) -> Venus encoder
 * and/or v4l2loopback.  AE/AWB run on GPU-derived scene statistics.
 */
#include "elish_camera.h"

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* PNG writer (RGB888, zlib)                                          */
/* ------------------------------------------------------------------ */

static uint32_t crc_table[256];
static int crc_table_ready;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_table_ready = 1;
}

static uint32_t crc_calc(const uint8_t *buf, size_t len)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int png_write(const char *path, int w, int h, const uint8_t *rgb, int stride)
{
    if (!crc_table_ready)
        crc_init();

    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_len);
    if (!raw)
        return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *dst = raw + (size_t)y * (1 + (size_t)w * 3);
        dst[0] = 0; /* filter: none */
        memcpy(dst + 1, rgb + (size_t)y * stride, (size_t)w * 3);
    }
    uLongf zlen = compressBound(raw_len);
    uint8_t *zdata = malloc(zlen);
    if (!zdata) {
        free(raw);
        return -1;
    }
    if (compress2(zdata, &zlen, raw, raw_len, 6) != Z_OK) {
        free(raw);
        free(zdata);
        return -1;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(zdata);
        return -1;
    }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    put_u32be(ihdr, (uint32_t)w);
    put_u32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type RGB */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */

    struct {
        const char *type;
        const uint8_t *data;
        uint32_t len;
    } chunks[2] = {
        { "IHDR", ihdr, 13 },
        { "IDAT", zdata, (uint32_t)zlen },
    };
    for (int i = 0; i < 2; i++) {
        uint8_t hdr[8];
        put_u32be(hdr, chunks[i].len);
        memcpy(hdr + 4, chunks[i].type, 4);
        uint8_t *crcbuf = malloc(4 + chunks[i].len);
        if (!crcbuf)
            break;
        memcpy(crcbuf, chunks[i].type, 4);
        memcpy(crcbuf + 4, chunks[i].data, chunks[i].len);
        uint32_t crc = crc_calc(crcbuf, 4 + chunks[i].len);
        free(crcbuf);
        uint8_t tail[4];
        put_u32be(tail, crc);
        fwrite(hdr, 1, 8, f);
        fwrite(chunks[i].data, 1, chunks[i].len, f);
        fwrite(tail, 1, 4, f);
    }
    static const uint8_t iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82 };
    fwrite(iend, 1, 12, f);
    fclose(f);
    free(zdata);
    return 0;
}


/* ------------------------------------------------------------------ */
/* loopback writer thread: decouples the (slow) v4l2loopback copy from */
/* the recording path.  The camera loop hands over NV12 buffers; the   */
/* writer drops frames if the consumer cannot keep up.                 */
/* ------------------------------------------------------------------ */

typedef struct {
    elish_loopback *lb;
    int width, height, stride;
    uint8_t *buf[2];
    int free_idx[2];
    int nfree;
    int pending; /* buffer index or -1 */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;
    uint64_t pushed;
    uint64_t dropped;
    pthread_t started;
} lbw_t;

static void *lbw_thread(void *arg)
{
    lbw_t *w = arg;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (w->pending < 0 && !w->stop)
            pthread_cond_wait(&w->cv, &w->mu);
        if (w->stop && w->pending < 0) {
            pthread_mutex_unlock(&w->mu);
            break;
        }
        int idx = w->pending;
        w->pending = -1;
        pthread_mutex_unlock(&w->mu);

        if (elish_lb_push_nv12(w->lb, w->buf[idx], w->stride) == 0)
            w->pushed++;
        else
            w->dropped++;

        pthread_mutex_lock(&w->mu);
        w->free_idx[w->nfree++] = idx;
        pthread_mutex_unlock(&w->mu);
    }
    return NULL;
}

static int lbw_init(lbw_t *w, const char *dev, int width, int height, int fps)
{
    memset(w, 0, sizeof(*w));
    w->width = width;
    w->height = height;
    w->stride = width;
    w->pending = -1;
    w->lb = elish_lb_open(dev, width, height, fps);
    if (!w->lb)
        return -1;
    size_t sz = (size_t)width * height * 3 / 2;
    for (int i = 0; i < 2; i++) {
        w->buf[i] = malloc(sz);
        if (!w->buf[i])
            return -1;
        w->free_idx[w->nfree++] = i;
    }
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    if (pthread_create(&w->started, NULL, lbw_thread, w) != 0)
        return -1;
    return 0;
}

/* Returns a buffer to render into, or -1 if the writer is still busy. */
static int lbw_acquire(lbw_t *w)
{
    pthread_mutex_lock(&w->mu);
    int idx = w->nfree > 0 ? w->free_idx[--w->nfree] : -1;
    pthread_mutex_unlock(&w->mu);
    return idx;
}

static void lbw_submit(lbw_t *w, int idx)
{
    pthread_mutex_lock(&w->mu);
    if (w->pending >= 0) {
        /* consumer is behind: drop the older frame, keep the newest */
        w->free_idx[w->nfree++] = w->pending;
        w->dropped++;
    }
    w->pending = idx;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static void lbw_stop(lbw_t *w)
{
    if (!w->lb)
        return;
    pthread_mutex_lock(&w->mu);
    w->stop = true;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->started, NULL);
    ELISH_LOG("loopback closed: %llu frames pushed, %llu drops",
              (unsigned long long)w->pushed, (unsigned long long)w->dropped);
    elish_lb_close(w->lb);
    for (int i = 0; i < 2; i++)
        free(w->buf[i]);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    w->lb = NULL;
}

/* ------------------------------------------------------------------ */
/* AE / AWB                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int exposure;
    int again;
    int dgain;
    elish_wb wb;
    float target_y; /* 0..255 */
    int min_exp, max_exp;
    int min_again, max_again;
    int min_dgain, max_dgain;
} aec_t;

static aec_t aec_init(const elish_cam_info *info)
{
    aec_t a;
    memset(&a, 0, sizeof(a));
    a.wb = (elish_wb){ 1.0f, 1.0f, 1.0f };
    /* linear-domain target ~18% grey (see gpu_debayer stats contract) */
    a.target_y = 46.0f;
    if (info->id == ELISH_CAM_FRONT) {
        a.exposure = 1508;
        a.again = 1536;
        a.dgain = 2048;
    } else {
        a.exposure = 1590;
        a.again = 1984;
        a.dgain = 4095;
    }
    a.max_exp = info->exposure_max > 0 ? info->exposure_max : 2400;
    a.min_exp = info->exposure_min > 0 ? info->exposure_min : 20;
    a.max_again = info->analogue_gain_max > 0 ? info->analogue_gain_max : 2047;
    a.min_again = info->analogue_gain_min > 0 ? info->analogue_gain_min : 128;
    a.max_dgain = info->digital_gain_max > 0 ? info->digital_gain_max : 4095;
    a.min_dgain = info->digital_gain_min > 0 ? info->digital_gain_min : 1024;
    if (a.exposure > a.max_exp) a.exposure = a.max_exp;
    if (a.again > a.max_again) a.again = a.max_again;
    if (a.dgain > a.max_dgain) a.dgain = a.max_dgain;
    return a;
}

static void aec_update(aec_t *a, const elish_stats *s, elish_camera *cam)
{
    /* AWB: grey world, tuned slowly. */
    if (s->mean_r > 1.0f && s->mean_b > 1.0f && s->mean_g > 1.0f) {
        float gr = s->mean_g / s->mean_r;
        float gb = s->mean_g / s->mean_b;
        a->wb.r *= 1.0f + 0.25f * (gr - 1.0f);
        a->wb.b *= 1.0f + 0.25f * (gb - 1.0f);
        if (a->wb.r < 0.4f) a->wb.r = 0.4f;
        if (a->wb.r > 4.0f) a->wb.r = 4.0f;
        if (a->wb.b < 0.4f) a->wb.b = 0.4f;
        if (a->wb.b > 4.0f) a->wb.b = 4.0f;
    }

    /* AE: aim for target luma.  Use exposure first, then analogue gain. */
    float mean = s->mean_y > 1.0f ? s->mean_y : 1.0f;
    float err = a->target_y / mean;
    if (err > 1.05f) {
        float f = err > 1.6f ? 1.6f : err;
        if (a->exposure < a->max_exp) {
            a->exposure = (int)(a->exposure * f);
            if (a->exposure > a->max_exp) a->exposure = a->max_exp;
        } else if (a->again < a->max_again) {
            a->again = (int)(a->again * f);
            if (a->again > a->max_again) a->again = a->max_again;
        } else {
            a->dgain = (int)(a->dgain * f);
            if (a->dgain > a->max_dgain) a->dgain = a->max_dgain;
        }
    } else if (err < 0.95f) {
        float f = err < 0.6f ? 0.6f : err;
        if (a->dgain > a->min_dgain + 32) {
            a->dgain = (int)(a->dgain * f);
            if (a->dgain < a->min_dgain) a->dgain = a->min_dgain;
        } else if (a->again > a->min_again + 16) {
            a->again = (int)(a->again * f);
            if (a->again < a->min_again) a->again = a->min_again;
        } else {
            a->exposure = (int)(a->exposure * f);
            if (a->exposure < a->min_exp) a->exposure = a->min_exp;
        }
    }
    elish_cam_set_exposure(cam, a->exposure, a->again, a->dgain);
}

/* ------------------------------------------------------------------ */
/* commands                                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Sensor modes: the V4L2 loopback format is producer-controlled, so the  */
/* resolution list has to be validated against real sensor modes.         */
/* ------------------------------------------------------------------ */

struct cam_mode {
    int w, h, fps;
};

static const struct cam_mode front_modes[] = {
    { 3280, 2464, 30 },
    { 1640, 1232, 30 },
};
static const struct cam_mode rear_modes[] = {
    { 4208, 3120, 30 },
    { 4160, 3120, 30 },
    { 4160, 2340, 30 },
    { 2104, 1560, 60 },
    { 2080, 1170, 60 },
    { 1364, 768, 120 },
};

static const struct cam_mode *modes_for(elish_cam_id id, int *count)
{
    if (id == ELISH_CAM_FRONT) {
        *count = (int)(sizeof(front_modes) / sizeof(front_modes[0]));
        return front_modes;
    }
    *count = (int)(sizeof(rear_modes) / sizeof(rear_modes[0]));
    return rear_modes;
}

static int mode_fps(elish_cam_id id, int w, int h)
{
    int n = 0;
    const struct cam_mode *m = modes_for(id, &n);
    for (int i = 0; i < n; i++)
        if (m[i].w == w && m[i].h == h)
            return m[i].fps;
    return 30;
}


/*
 * Current display orientation, used to rotate the camera stream so the image
 * is upright for the way the tablet is held.  Mutter's DisplayConfig reports a
 * "transform" per logical monitor: 0 = normal, 1 = 90, 2 = 180, 3 = 270.
 * Fallback: xrandr geometry (portrait when height > width).
 */
static int display_transform(void)
{
    FILE *f = popen("gdbus call --session --dest org.gnome.Mutter.DisplayConfig "
                    "--object-path /org/gnome/Mutter/DisplayConfig "
                    "--method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null", "r");
    char buf[4096];
    size_t n = 0;
    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        pclose(f);
    }
    buf[n] = 0;
    /* serial is the first "uint32 N"; transform is the second one */
    int seen = 0;
    for (char *p = buf; (p = strstr(p, "uint32 ")); p += 7) {
        int v = atoi(p + 7);
        if (v < 0 || v > 3)
            continue;
        if (++seen == 2)
            return v;
    }
    /* fallback: xrandr current geometry */
    f = popen("xrandr --current 2>/dev/null", "r");
    int portrait = 0;
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            int w = 0, h = 0;
            if (strstr(line, " connected ") && sscanf(line, "%dx%d", &w, &h) == 2) {
                portrait = h > w;
                break;
            }
        }
        pclose(f);
    }
    return portrait ? 1 : 0;
}

/* Rotation (clockwise degrees) for landscape ("transform 0") displays.
 * Measured on hardware: both modules are mounted with the same orientation
 * (front 180, rear 180) relative to the device's landscape frame. */
static int default_rotation(elish_cam_id id, int transform)
{
    int base = 180; /* front and rear both 180 in landscape */
    (void)id;
    int delta;
    switch (transform & 3) {
    case 1: delta = 270; break;  /* portrait, rotated 90  */
    case 2: delta = 180; break;  /* upside-down landscape */
    case 3: delta = 90;  break;  /* portrait, rotated 270 */
    default: delta = 0; break;
    }
    return (base + delta) % 360;
}

/* Read a key from /etc/elish-orientation.conf (shared with the
 * elish-tablet-mode helper and the elish-camera-ctl config tool). */
static int read_orientation_config(const char *key, char *out, size_t out_len)
{
    FILE *f = fopen("/etc/elish-orientation.conf", "r");
    if (!f)
        return -1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '=');
        if (!p) {
            if (line[0] != '#' && line[0] != '\n')
                continue;
            continue;
        }
        *p++ = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
            *--end = 0;
        if (!strcmp(line, key)) {
            snprintf(out, out_len, "%s", p);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/* 0 = idle-only rebuild, 1 = live rebuild (+ restart Snapshot), 2 = off */
static int rebuild_mode(void)
{
    char v[32];
    if (read_orientation_config("rebuild_mode", v, sizeof(v)) < 0)
        return 0;
    if (!strcmp(v, "live"))
        return 1;
    if (!strcmp(v, "off"))
        return 2;
    return 0;
}

static int snapshot_running(void)
{
    return system("pgrep -x snapshot >/dev/null 2>&1") == 0;
}

static void restart_snapshot(void)
{
    ELISH_LOG("autoserve: restarting GNOME Snapshot to pick up the new stream format");
    system("flatpak kill org.gnome.Snapshot >/dev/null 2>&1");
    usleep(600 * 1000);
    system("setsid nohup flatpak run org.gnome.Snapshot >/dev/null 2>&1 &");
}

/* ------------------------------------------------------------------ */
/* Autofocus (rear CN3927V VCM; front camera has fixed focus)          */
/* ------------------------------------------------------------------ */

static int g_focus_pos = 512;

/*
 * Focus metric: mean squared Laplacian over the whole Y plane (subsampled).
 * A centre crop is misleading when the middle of the scene is flat (white
 * paper/ceiling); the full frame still contains enough texture to rank focus.
 */
static double nv12_sharpness(const uint8_t *nv, int w, int h, int stride)
{
    long long acc = 0;
    long cnt = 0;
    for (int y = 1; y < h - 1; y += 3) {
        const uint8_t *row = nv + (size_t)y * stride;
        const uint8_t *up = row - stride;
        const uint8_t *dn = row + stride;
        for (int x = 1; x < w - 1; x += 3) {
            int lap = 4 * (int)row[x] - (int)up[x] - (int)dn[x] - (int)row[x - 1] -
                      (int)row[x + 1];
            acc += (long long)lap * lap;
            cnt++;
        }
    }
    return cnt > 0 ? (double)acc / cnt : 0.0;
}

/* Measure mean sharpness at the current focus position (settles 2 frames). */
static double af_measure(elish_camera *cam, elish_gpu *gpu, const elish_cam_info *info,
                         int out_w, int out_h, int rot, elish_wb wb, uint8_t *nv12, int nv12_stride)
{
    double best = -1;
    for (int i = 0; i < 4; i++) {
        elish_raw_image img;
        if (elish_cam_get_frame(cam, &img, NULL) < 0)
            return best;
        if (i >= 2 && elish_gpu_debayer_nv12(gpu, &img, info->bayer, out_w, out_h, rot, wb,
                                             1.0f / 2.2f, nv12, nv12_stride) == 0) {
            double sc = nv12_sharpness(nv12, out_w, out_h, nv12_stride);
            if (sc > best)
                best = sc;
        }
        elish_cam_put_frame(cam);
    }
    return best;
}

/*
 * Sweep `steps` positions in [center-range, center+range] and return the focus
 * position with the sharpest frame.  Updates g_focus_pos.
 */
static int af_sweep(elish_camera *cam, elish_gpu *gpu, const elish_cam_info *info,
                    int out_w, int out_h, int rot, elish_wb wb, int center, int range, int steps,
                    double *best_score)
{
    uint8_t *nv12 = malloc((size_t)out_w * out_h * 3 / 2);
    if (!nv12)
        return g_focus_pos;
    int best_pos = center;
    double best = -1;
    for (int i = 0; i < steps; i++) {
        int pos = center;
        if (steps > 1)
            pos = center - range + (2 * range * i) / (steps - 1);
        if (pos < 0) pos = 0;
        if (pos > 1023) pos = 1023;
        elish_cam_set_focus(cam, pos);
        usleep(20 * 1000);
        double sc = af_measure(cam, gpu, info, out_w, out_h, rot, wb, nv12, out_w);
        if (sc > best) {
            best = sc;
            best_pos = pos;
        }
    }
    free(nv12);
    if (best <= 0) {
        if (best_score)
            *best_score = -1;
        return g_focus_pos; /* no usable frame: keep current focus */
    }
    g_focus_pos = best_pos;
    elish_cam_set_focus(cam, best_pos);
    if (best_score)
        *best_score = best;
    return best_pos;
}

struct opts {
    elish_cam_id cam;
    int width, height;
    int fps;
    int rotate;
    int duration;
    int frames;
    const char *output;
    const char *loopback; /* --loopback virtual camera device */
    elish_codec codec;
    int bitrate_kbps;
    int keyint;
    int focus;
    bool have_focus;
    bool autofocus;
    bool fps_set;
    bool no_ae;
    int fw, fh, ffps;   /* autoserve: front device */
    int rw, rh, rfps;   /* autoserve: rear device  */
    int frot, rrot;     /* autoserve: -1 = auto from display orientation */
};

/* Snap a requested size to a real sensor mode (exact preferred, otherwise nearest). */
static int snap_mode(struct opts *o)
{
    int n = 0;
    const struct cam_mode *m = modes_for(o->cam, &n);
    if (o->width <= 0 || o->height <= 0) {
        o->width = m[0].w;
        o->height = m[0].h;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (m[i].w == o->width && m[i].h == o->height)
            return 0;
    }
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < n; i++) {
        int d = abs(m[i].w - o->width) + abs(m[i].h - o->height);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    fprintf(stderr,
            "elish-camera: %dx%d is not a sensor mode; using %dx%d (supported:",
            o->width, o->height, m[best].w, m[best].h);
    for (int i = 0; i < n; i++)
        fprintf(stderr, " %dx%d@%d", m[i].w, m[i].h, m[i].fps);
    fprintf(stderr, ")\n");
    o->width = m[best].w;
    o->height = m[best].h;
    return 0;
}


static void usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  elish-camera list\n"
            "  elish-camera photo  --camera front|rear -o out.png [--size WxH] [--rotate 0|90|180|270]\n"
            "                      [--focus 0..1023]\n"
            "  elish-camera record --camera front|rear -o out.mp4 [-d SECONDS] [--size WxH] [--fps 30]\n"
            "                      [--codec hevc|h264] [--bitrate KBPS] [--keyint N] [--loopback DEV]\n"
            "                      [--rotate DEG] [--focus N] [--frames N] [--no-ae]\n"
            "  elish-camera stream --camera front|rear --loopback DEV [--size WxH] [--fps 30]\n"
            "  elish-camera serve --camera front|rear [--loopback DEV] [--size WxH] [--fps NV]\n"
            "                     [--rotate DEG] [--duration SEC] [--autofocus] [--no-ae]\n"
            "                     (keeps the camera available as /dev/video2 front, /dev/video20 rear)\n"
            "  elish-camera autoserve [--front-size WxH] [--front-fps N]\n"
            "                         [--rear-size WxH] [--rear-fps N] [--autofocus]\n"
            "                     (both /dev/video2 and /dev/video20 stay available; the\n"
            "                      backend switches the sensor to whichever device is opened)\n"
            "  elish-camera focus --camera rear [--size WxH]   (one-shot autofocus sweep)\n"
            "\n"
            "sensor modes: front 3280x2464@30 1640x1232@30;\n"
            "              rear 4208x3120@30 4160x3120@30 4160x2340@30 2104x1560@60 2080x1170@60 1364x768@120\n"
            "  --size snaps to the nearest real sensor mode; --fps defaults to the mode rate and drops frames if lower.\n"
            "  --autofocus: rear only (front camera is fixed focus); runs a sweep before photo/record/serve.\n"
            "                      [--duration SEC] [--rotate DEG] [--focus N] [--no-ae]\n"
            "\n"
            "defaults: front 3280x2464 rot 180, rear 4208x3120 rot 270, 30fps, HEVC 20Mbps;\n"
            "record/stream run until Ctrl-C unless -d/-n is given\n");
}

static int parse_args(int argc, char **argv, struct opts *o)
{
    memset(o, 0, sizeof(*o));
    o->cam = ELISH_CAM_REAR;
    o->fps = 30;
    o->rotate = -1;
    o->codec = ELISH_CODEC_HEVC;
    o->bitrate_kbps = 20000;
    o->keyint = 30;
    o->duration = 0; /* 0 = run until SIGINT */
    o->focus = -1;
    o->frot = -1;
    o->rrot = -1;

    static const struct option longopts[] = {
        { "camera", required_argument, 0, 'c' },
        { "size", required_argument, 0, 's' },
        { "fps", required_argument, 0, 'f' },
        { "rotate", required_argument, 0, 'r' },
        { "duration", required_argument, 0, 'd' },
        { "frames", required_argument, 0, 'n' },
        { "output", required_argument, 0, 'o' },
        { "loopback", required_argument, 0, 'l' },
        { "codec", required_argument, 0, 'x' },
        { "bitrate", required_argument, 0, 'b' },
        { "keyint", required_argument, 0, 'k' },
        { "focus", required_argument, 0, 'F' },
        { "rotate-front", required_argument, 0, 1005 },
        { "rotate-rear", required_argument, 0, 1006 },
        { "front-size", required_argument, 0, 1001 },
        { "front-fps", required_argument, 0, 1002 },
        { "rear-size", required_argument, 0, 1003 },
        { "rear-fps", required_argument, 0, 1004 },
        { "autofocus", no_argument, 0, 'a' },
        { "no-ae", no_argument, 0, 'A' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "c:s:f:r:d:n:o:l:x:b:k:F:aAh", longopts, NULL)) != -1) {
        switch (c) {
        case 'c':
            o->cam = !strcmp(optarg, "front") ? ELISH_CAM_FRONT : ELISH_CAM_REAR;
            break;
        case 's':
            if (sscanf(optarg, "%dx%d", &o->width, &o->height) != 2)
                return -1;
            break;
        case 'f': o->fps = atoi(optarg); o->fps_set = true; break;
        case 'r': o->rotate = atoi(optarg); break;
        case 'd': o->duration = atoi(optarg); break;
        case 'n': o->frames = atoi(optarg); break;
        case 'o': o->output = optarg; break;
        case 'l': o->loopback = optarg; break;
        case 'x': o->codec = !strcmp(optarg, "h264") ? ELISH_CODEC_H264 : ELISH_CODEC_HEVC; break;
        case 'b': o->bitrate_kbps = atoi(optarg); break;
        case 'k': o->keyint = atoi(optarg); break;
        case 'F': o->focus = atoi(optarg); o->have_focus = true; break;
        case 'a': o->autofocus = true; break;
        case 1001: if (sscanf(optarg, "%dx%d", &o->fw, &o->fh) != 2) return -1; break;
        case 1002: o->ffps = atoi(optarg); break;
        case 1003: if (sscanf(optarg, "%dx%d", &o->rw, &o->rh) != 2) return -1; break;
        case 1004: o->rfps = atoi(optarg); break;
        case 1005: o->frot = atoi(optarg); break;
        case 1006: o->rrot = atoi(optarg); break;
        case 'A': o->no_ae = true; break;
        case 'h': usage(); exit(0);
        default: return -1;
        }
    }
    if (snap_mode(o) < 0)
        return -1;
    if (!o->fps_set)
        o->fps = mode_fps(o->cam, o->width, o->height);
    int mfps = mode_fps(o->cam, o->width, o->height);
    if (o->fps > mfps) {
        fprintf(stderr, "elish-camera: fps %d clamped to sensor mode rate %d\n", o->fps, mfps);
        o->fps = mfps;
    }
    if (o->fw <= 0) { o->fw = 3280; o->fh = 2464; }
    if (o->rw <= 0) { o->rw = 4208; o->rh = 3120; }
    if (o->ffps <= 0) o->ffps = 30;
    if (o->rfps <= 0) o->rfps = 30;
    if (o->rotate < 0)
        o->rotate = default_rotation(o->cam, display_transform());
    if (o->keyint <= 0)
        o->keyint = o->fps > 0 ? o->fps : 30;
    if (o->fps <= 0)
        o->fps = 30;
    if (o->bitrate_kbps <= 0)
        o->bitrate_kbps = 20000;
    return 0;
}

static int cmd_list(void)
{
    for (int i = 0; i < 2; i++) {
        elish_cam_id id = (elish_cam_id)i;
        elish_camera *cam = elish_cam_open(id, 0, 0, 30);
        if (!cam) {
            printf("%-5s : unavailable\n", elish_cam_name(id));
            continue;
        }
        const elish_cam_info *info = elish_cam_info_get(cam);
        printf("%-5s : %dx%d bayer=%s sensor=%s video=%s\n", elish_cam_name(id),
               info->width, info->height, elish_bayer_name(info->bayer),
               info->sensor_subdev, info->video_dev);
        elish_cam_close(cam);
    }
    return 0;
}

/* Open camera, settle AE/AWB for `warmup` frames; leaves camera streaming. */
static elish_camera *camera_ready(const struct opts *o, aec_t *aec, elish_gpu *gpu, int warmup)
{
    for (int attempt = 0; attempt < 3 && !g_stop; attempt++) {
        if (attempt > 0) {
            ELISH_LOG("camera open/start retry %d", attempt);
            usleep(400 * 1000);
        }
        elish_camera *cam = elish_cam_open(o->cam, o->width, o->height, o->fps);
        if (!cam)
            continue;
        const elish_cam_info *info = elish_cam_info_get(cam);
        if (o->have_focus)
            elish_cam_set_focus(cam, o->focus);
        if (elish_cam_start(cam) < 0) {
            elish_cam_close(cam);
            continue;
        }
        *aec = aec_init(info);
        int ok = 1;
        for (int i = 0; i < warmup && !g_stop; i++) {
            elish_raw_image img;
            if (elish_cam_get_frame(cam, &img, NULL) < 0) {
                ok = 0;
                break;
            }
            if (!o->no_ae && i % 3 == 0) {
                elish_stats st;
                if (elish_gpu_stats(gpu, &img, info->bayer, aec->wb, &st) == 0)
                    aec_update(aec, &st, cam);
            }
            elish_cam_put_frame(cam);
        }
        if (ok) {
            if (o->autofocus && o->cam == ELISH_CAM_REAR) {
                int out_w = info->width, out_h = info->height;
                if (o->rotate == 90 || o->rotate == 270) {
                    int t2 = out_w; out_w = out_h; out_h = t2;
                }
                ELISH_LOG("autofocus: sweeping VCM (rear) ...");
                double sc = 0;
                int pos = af_sweep(cam, gpu, info, out_w, out_h, o->rotate, aec->wb, 512, 511, 9, &sc);
                ELISH_LOG("autofocus: coarse best=%d (score %.2f), refining", pos, sc);
                pos = af_sweep(cam, gpu, info, out_w, out_h, o->rotate, aec->wb, pos, 80, 5, &sc);
                ELISH_LOG("autofocus: final focus=%d (score %.2f)", pos, sc);
            }
            return cam;
        }
        ELISH_ERR("camera %s delivered no frames, reconfiguring", elish_cam_name(o->cam));
        elish_cam_stop(cam);
        elish_cam_close(cam);
    }
    return NULL;
}

static int write_photo(elish_gpu *gpu, elish_camera *cam, const struct opts *o, const aec_t *aec)
{
    const elish_cam_info *info = elish_cam_info_get(cam);
    int out_w = o->width ? o->width : info->width;
    int out_h = o->height ? o->height : info->height;
    int rot = o->rotate;
    if (rot == 90 || rot == 270) {
        int t = out_w;
        out_w = out_h;
        out_h = t;
    }
    uint8_t *rgb = malloc((size_t)out_w * out_h * 3);
    if (!rgb)
        return -1;
    elish_raw_image img;
    if (elish_cam_get_frame(cam, &img, NULL) < 0) {
        free(rgb);
        return -1;
    }
    int rc = elish_gpu_debayer_rgb(gpu, &img, info->bayer, out_w, out_h, rot, aec->wb, 1.0f / 2.2f,
                                   rgb, out_w * 3);
    elish_cam_put_frame(cam);
    if (rc < 0) {
        free(rgb);
        return -1;
    }
    const char *path = o->output ? o->output : "photo.png";
    rc = png_write(path, out_w, out_h, rgb, out_w * 3);
    free(rgb);
    if (rc == 0)
        printf("photo: %s %dx%d rotate=%d wb=(%.2f,1.00,%.2f)\n", path, out_w, out_h, rot,
               aec->wb.r, aec->wb.b);
    return rc;
}

static int run_output(const struct opts *o, bool recording, bool streaming)
{
    elish_gpu *gpu = elish_gpu_create();
    if (!gpu) {
        ELISH_ERR("GPU init failed");
        return 1;
    }
    printf("GPU renderer: %s\n", elish_gpu_renderer(gpu));

    elish_encoder *enc = NULL;
    char es_path[512] = "";

    aec_t aec;
    elish_camera *cam = camera_ready(o, &aec, gpu, 6);
    if (!cam) {
        ELISH_ERR("camera open failed");
        elish_gpu_destroy(gpu);
        return 1;
    }
    const elish_cam_info *info = elish_cam_info_get(cam);

    int out_w = o->width ? o->width : info->width;
    int out_h = o->height ? o->height : info->height;
    if (o->rotate == 90 || o->rotate == 270) {
        int t = out_w;
        out_w = out_h;
        out_h = t;
    }

    if (recording) {
        snprintf(es_path, sizeof(es_path), "%s.es", o->output ? o->output : "record.mp4");
        enc = elish_enc_create(o->codec, out_w, out_h, o->fps, o->bitrate_kbps * 1000, es_path);
        if (!enc) {
            ELISH_ERR("encoder init failed");
            goto out;
        }
        if (elish_enc_start(enc) < 0) {
            ELISH_ERR("encoder start failed");
            goto out;
        }
    }
    lbw_t lbw;
    memset(&lbw, 0, sizeof(lbw));
    if (streaming) {
        if (!o->loopback) {
            ELISH_ERR("stream requires --loopback /dev/videoN");
            goto out;
        }
        if (lbw_init(&lbw, o->loopback, out_w, out_h, o->fps) < 0) {
            ELISH_ERR("loopback open failed");
            goto out;
        }
    }

    const int nv12_stride = out_w;
    const int src_fps = mode_fps(o->cam, info->width, info->height);
    int drop_div = 1;
    if (o->fps > 0 && src_fps > o->fps)
        drop_div = (src_fps + o->fps / 2) / o->fps;
    if (drop_div < 1)
        drop_div = 1;
    uint64_t last_af_ns = 0;
    uint8_t *nv12 = malloc((size_t)nv12_stride * out_h * 3 / 2);
    if (!nv12) {
        ELISH_ERR("out of memory");
        goto out;
    }

    uint64_t t_start = elish_now_ns();
    int frames = 0, pushed = 0;
    uint64_t t_last = t_start;
    uint64_t t_gpu = 0, t_enc = 0, t_lb = 0, t_cap = 0;
    const bool profile = getenv("ELISH_PROFILE") != NULL;
    while (!g_stop) {
        if (o->frames > 0 && frames >= o->frames)
            break;
        if (!recording && !streaming && o->duration > 0 &&
            (elish_now_ns() - t_start) / 1000000000ull >= (uint64_t)o->duration)
            break;
        if (recording && o->duration > 0 &&
            (elish_now_ns() - t_start) / 1000000000ull >= (uint64_t)o->duration)
            break;

        elish_raw_image img;
        uint64_t ts = 0;
        uint64_t tp0 = profile ? elish_now_ns() : 0;
        if (elish_cam_get_frame(cam, &img, &ts) < 0)
            break;
        uint64_t tp1 = profile ? elish_now_ns() : 0;
        if (!o->no_ae && frames % 8 == 0) {
            elish_stats st;
            if (elish_gpu_stats(gpu, &img, info->bayer, aec.wb, &st) == 0)
                aec_update(&aec, &st, cam);
        }
        /* frame-rate limiting: drop source frames when --fps < sensor mode rate */
        if (drop_div > 1 && frames % drop_div != 0) {
            elish_cam_put_frame(cam);
            frames++;
            continue;
        }
        int lb_idx = streaming ? lbw_acquire(&lbw) : -1;
        uint8_t *dst = lb_idx >= 0 ? lbw.buf[lb_idx] : nv12;
        int dst_stride = lb_idx >= 0 ? lbw.stride : nv12_stride;
        uint64_t tg0 = profile ? elish_now_ns() : 0;
        int gpu_ok = elish_gpu_debayer_nv12(gpu, &img, info->bayer, out_w, out_h, o->rotate,
                                            aec.wb, 1.0f / 2.2f, dst, dst_stride);
        uint64_t tg1 = profile ? elish_now_ns() : 0;
        if (gpu_ok == 0) {
            uint64_t te0 = profile ? elish_now_ns() : 0;
            if (enc && elish_enc_push_nv12(enc, dst, dst_stride, ts,
                                           frames % o->keyint == 0) == 0)
                pushed++;
            uint64_t te1 = profile ? elish_now_ns() : 0;
            if (profile) {
                t_cap += tp1 - tp0;
                t_gpu += tg1 - tg0;
                t_enc += te1 - te0;
            }
            if (lb_idx >= 0)
                lbw_submit(&lbw, lb_idx);
        } else if (lb_idx >= 0) {
            lbw_submit(&lbw, lb_idx);
        }
        elish_cam_put_frame(cam);
        frames++;
        /* periodic autofocus must run with the frame returned to the driver */
        if (o->autofocus && o->cam == ELISH_CAM_REAR && nv12 &&
            elish_now_ns() - last_af_ns > 20ull * 1000000000ull) {
            last_af_ns = elish_now_ns();
            double sc = 0;
            int pos = af_sweep(cam, gpu, info, out_w, out_h, o->rotate, aec.wb, g_focus_pos,
                               160, 5, &sc);
            ELISH_LOG("autofocus: focus=%d score=%.1f", pos, sc);
        }
        uint64_t now = elish_now_ns();
        if (frames % 30 == 0) {
            double dt = (double)(now - t_last) / 1e9;
            printf("frames=%d fps=%.1f exposure=%d again=%d dgain=%d wb=(%.2f,1.00,%.2f)%s\n",
                   frames, 30.0 / (dt > 0 ? dt : 1), aec.exposure, aec.again, aec.dgain,
                   aec.wb.r, aec.wb.b,
                   o->cam == ELISH_CAM_REAR ? " focus=see autofocus log" : "");
            if (profile) {
                int n = 30;
                printf("  profile: cap=%.2fms gpu=%.2fms enc=%.2fms lb=%.2fms\n",
                       t_cap / 1e6 / n, t_gpu / 1e6 / n, t_enc / 1e6 / n, t_lb / 1e6 / n);
                t_cap = t_gpu = t_enc = t_lb = 0;
            }
            fflush(stdout);
            t_last = now;
        }
    }
    double total = (double)(elish_now_ns() - t_start) / 1e9;
    printf("done: %d source frames in %.2fs (capture %.2f fps), encoded %d (output %.2f fps)\n",
           frames, total, total > 0 ? frames / total : 0, pushed,
           total > 0 ? pushed / total : 0);

    free(nv12);
    if (enc) {
        elish_enc_stop(enc);
        if (o->output) {
            char mp4[520];
            snprintf(mp4, sizeof(mp4), "%s", o->output);
            /* use the measured capture rate so the MP4 timeline matches
             * real time even when frames were dropped under load */
            int mux_fps = o->fps;
            if (total > 0 && pushed > 1) {
                mux_fps = (int)(pushed / total + 0.5);
                if (mux_fps < 1) mux_fps = 1;
            }
            char fpsbuf[16];
            snprintf(fpsbuf, sizeof(fpsbuf), "%d", mux_fps);
            setenv("ELISH_MUX_FPS", fpsbuf, 1);
            if (elish_mux_mp4(es_path, mp4, o->codec) == 0)
                printf("video: %s\n", mp4);
        }
    }
out:
    if (enc)
        elish_enc_destroy(enc);
    lbw_stop(&lbw);
    elish_cam_stop(cam);
    elish_cam_close(cam);
    elish_gpu_destroy(gpu);
    return 0;
}


/* ------------------------------------------------------------------ */
/* serve: keep one camera permanently available as a standard V4L2    */
/* device (/dev/video2 for front, /dev/video20 for rear).             */
/* Note: the two sensors cannot stream simultaneously on this board   */
/* (starting one stream stalls the other - measured), so third-party  */
/* apps see one camera at a time; stop/start serve to switch.         */
/* ------------------------------------------------------------------ */

static const char *serve_loopback_for(elish_cam_id id)
{
    return id == ELISH_CAM_FRONT ? "/dev/video2" : "/dev/video20";
}

static int cmd_serve(struct opts *o)
{
    if (!o->loopback)
        o->loopback = serve_loopback_for(o->cam);
    o->output = NULL;
    ELISH_LOG("serve: camera=%s loopback=%s %dx%d rotate=%d fps=%d",
              elish_cam_name(o->cam), o->loopback,
              o->width ? o->width : (o->cam == ELISH_CAM_FRONT ? 3280 : 4208),
              o->height ? o->height : (o->cam == ELISH_CAM_FRONT ? 2464 : 3120),
              o->rotate, o->fps);
    return run_output(o, false, true);
}


/* ------------------------------------------------------------------ */
/* focus: one-shot autofocus sweep (rear camera only)                  */
/* ------------------------------------------------------------------ */

static int cmd_focus(struct opts *o)
{
    if (o->cam != ELISH_CAM_REAR) {
        ELISH_ERR("front camera OV8856 has a fixed-focus lens: no autofocus hardware");
        return 1;
    }
    elish_gpu *gpu = elish_gpu_create();
    if (!gpu) {
        ELISH_ERR("GPU init failed");
        return 1;
    }
    elish_camera *cam = elish_cam_open(o->cam, o->width, o->height, o->fps);
    if (!cam) {
        elish_gpu_destroy(gpu);
        return 1;
    }
    const elish_cam_info *info = elish_cam_info_get(cam);
    if (elish_cam_start(cam) < 0) {
        elish_cam_close(cam);
        elish_gpu_destroy(gpu);
        return 1;
    }
    aec_t aec = aec_init(info);
    for (int i = 0; i < 8; i++) {
        elish_raw_image img;
        elish_stats st;
        if (elish_cam_get_frame(cam, &img, NULL) < 0)
            break;
        if (i % 2 == 0 && elish_gpu_stats(gpu, &img, info->bayer, aec.wb, &st) == 0)
            aec_update(&aec, &st, cam);
        elish_cam_put_frame(cam);
    }
    int out_w = info->width, out_h = info->height;
    if (o->rotate == 90 || o->rotate == 270) { int t = out_w; out_w = out_h; out_h = t; }
    int cur = 512;
    {
        FILE *f = fopen("/sys/bus/i2c/devices/20-000c/focus", "r");
        if (f) { if (fscanf(f, "%d", &cur) != 1) cur = 512; fclose(f); }
    }
    double sc = 0;
    int pos = af_sweep(cam, gpu, info, out_w, out_h, o->rotate, aec.wb, 512, 511, 9, &sc);
    ELISH_LOG("focus: coarse=%d score=%.1f, refining", pos, sc);
    pos = af_sweep(cam, gpu, info, out_w, out_h, o->rotate, aec.wb, pos, 80, 5, &sc);
    ELISH_LOG("focus: final=%d score=%.1f", pos, sc);
    elish_cam_stop(cam);
    elish_cam_close(cam);
    elish_gpu_destroy(gpu);
    printf("best focus=%d (write with: echo %d | sudo tee /sys/bus/i2c/devices/20-000c/focus)\n",
           pos, pos);
    return 0;
}


/* ------------------------------------------------------------------ */
/* autoserve: both cameras exist as standard V4L2 devices; the backend */
/* routes the (single) sensor to whichever device is being opened.     */
/*                                                                     */
/* The two sensors cannot stream at the same time (measured: starting  */
/* one stream stalls the other), but applications only stream one      */
/* device at a time, so opening /dev/video2 or /dev/video20 is enough  */
/* to select the camera - no manual switch needed.                     */
/* ------------------------------------------------------------------ */

struct as_dev {
    elish_cam_id id;
    const char *dev;
    int req_w, req_h;
    int out_w, out_h;
    int rot, fps;
    elish_loopback *lb;
};

/* number of *other* processes that have this device open */
static int proc_device_users(const char *devpath)
{
    DIR *d = opendir("/proc");
    int count = 0;
    pid_t self = getpid();
    if (!d)
        return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid == self)
            continue;
        char fddir[64];
        snprintf(fddir, sizeof(fddir), "/proc/%d/fd", (int)pid);
        DIR *fd = opendir(fddir);
        if (!fd)
            continue;
        struct dirent *fe;
        while ((fe = readdir(fd))) {
            if (fe->d_name[0] == '.')
                continue;
            char path[320], link[256];
            snprintf(path, sizeof(path), "/proc/%d/fd/%s", (int)pid, fe->d_name);
            ssize_t n = readlink(path, link, sizeof(link) - 1);
            if (n <= 0)
                continue;
            link[n] = 0;
            if (strcmp(link, devpath) == 0)
                count++;
        }
        closedir(fd);
    }
    closedir(d);
    return count;
}

static int cmd_autoserve(struct opts *o)
{
    struct as_dev devs[2];
    memset(devs, 0, sizeof(devs));
    devs[0] = (struct as_dev){ .id = ELISH_CAM_FRONT, .dev = "/dev/video2",
                               .req_w = o->fw, .req_h = o->fh, .fps = o->ffps };
    devs[1] = (struct as_dev){ .id = ELISH_CAM_REAR, .dev = "/dev/video20",
                               .req_w = o->rw, .req_h = o->rh, .fps = o->rfps };

    elish_gpu *gpu = elish_gpu_create();
    if (!gpu) {
        ELISH_ERR("autoserve: GPU init failed");
        return 1;
    }
    ELISH_LOG("autoserve: GPU renderer %s (display transform %d -> front rot %d, rear rot %d)",
              elish_gpu_renderer(gpu), display_transform(),
              default_rotation(ELISH_CAM_FRONT, display_transform()),
              default_rotation(ELISH_CAM_REAR, display_transform()));

    /* Keep both virtual devices present and pre-formatted so applications can
     * open either one at any time (they will just wait for frames until the
     * backend has switched the sensor). */
    for (int i = 0; i < 2; i++) {
        struct as_dev *d = &devs[i];
        int rot = (d->id == ELISH_CAM_FRONT && o->frot >= 0) ? o->frot
                : (d->id == ELISH_CAM_REAR && o->rrot >= 0) ? o->rrot
                : default_rotation(d->id, display_transform());
        int ow = d->req_w, oh = d->req_h;
        int sensor_fps = mode_fps(d->id, d->req_w, d->req_h);
        if (d->fps <= 0 || d->fps > sensor_fps)
            d->fps = sensor_fps;
        if (rot == 90 || rot == 270) {
            int t = ow;
            ow = oh;
            oh = t;
        }
        d->out_w = ow;
        d->out_h = oh;
        d->rot = rot;
        d->lb = elish_lb_open(d->dev, ow, oh, d->fps);
        if (!d->lb) {
            ELISH_ERR("autoserve: cannot open loopback %s", d->dev);
            return 1;
        }
    }

    /* Also publish real PipeWire camera nodes with proper format caps.
     * v4l2loopback nodes expose no EnumFormat on this stack, so GStreamer/
     * aperture apps (GNOME Snapshot etc.) would filter them out; these nodes
     * are visible through the portal camera fd and carry valid caps. */
    struct as_dev_pw {
        elish_pwcam *cam;
    } pw[2];
    memset(pw, 0, sizeof(pw));
    for (int i = 0; i < 2; i++) {
        struct as_dev *d = &devs[i];
        char name[64], desc[96];
        snprintf(name, sizeof(name), "elish-camera-%s",
                 d->id == ELISH_CAM_FRONT ? "front" : "rear");
        snprintf(desc, sizeof(desc), "Elish %s Camera",
                 d->id == ELISH_CAM_FRONT ? "Front" : "Rear");
        pw[i].cam = elish_pwcam_create(name, desc,
                                       d->id == ELISH_CAM_FRONT ? "front" : "back",
                                       d->out_w, d->out_h, d->fps);
        if (!pw[i].cam)
            ELISH_ERR("autoserve: cannot publish PipeWire node %s", name);
    }

    elish_camera *cam = NULL;
    elish_gpu *cam_gpu = NULL;
    (void)cam_gpu;
    aec_t aec;
    memset(&aec, 0, sizeof(aec));
    int active = -1;
    int cand = -1, cand_count = 0;
    int last_users[2] = { 0, 0 };
    int last_req[2] = { 0, 0 };
    uint8_t *nv12 = NULL;
    size_t nv12_size = 0;
    uint64_t last_scan = 0, last_af = elish_now_ns();
    int frames_since_switch = 0;
    int cur_transform = display_transform();
    int pending_transform = -1;
    uint64_t last_orient = elish_now_ns();
    int cur_rebuild_mode = rebuild_mode();

    while (!g_stop) {
        uint64_t now = elish_now_ns();
        /* Scan more often while a camera is actively streamed; idle scans are
         * cheaper on CPU and reduce latency spikes for the rest of the system. */
        uint64_t scan_interval = (active >= 0) ? 400ull * 1000 * 1000
                                               : 1000ull * 1000 * 1000;
        if (now - last_scan > scan_interval) {
            last_scan = now;
            int users[2] = { proc_device_users(devs[0].dev), proc_device_users(devs[1].dev) };
            int pwact[2] = { elish_pwcam_active(pw[0].cam) ? 1 : 0,
                             elish_pwcam_active(pw[1].cam) ? 1 : 0 };
            int req[2] = { users[0] + pwact[0], users[1] + pwact[1] };
            if (users[0] != last_users[0] || users[1] != last_users[1] ||
                req[0] != last_req[0] || req[1] != last_req[1]) {
                ELISH_LOG("autoserve: v4l2 front=%d rear=%d, pipewire front=%d rear=%d (active=%s)",
                          users[0], users[1], pwact[0], pwact[1],
                          active < 0 ? "none" : elish_cam_name(devs[active].id));
                last_req[0] = req[0];
                last_req[1] = req[1];
                last_users[0] = users[0];
                last_users[1] = users[1];
            }
            int want = -1; /* -1 = idle */
            if (active >= 0) {
                if (req[active] > 0)
                    want = active;
                else if (req[1 - active] > 0)
                    want = 1 - active;
                else
                    want = -1;
            } else if (req[0] > 0) {
                want = 0;
            } else if (req[1] > 0) {
                want = 1;
            }
            /* debounce transient opens (apps enumerate all /dev/video* nodes) */
            if (want == cand)
                cand_count++;
            else {
                cand = want;
                cand_count = 1;
            }
            if (cand_count >= 2 && cand != active) {
                if (cam) {
                    elish_cam_stop(cam);
                    elish_cam_close(cam);
                    cam = NULL;
                }
                if (cand >= 0) {
                    struct as_dev *d = &devs[cand];
                    ELISH_LOG("autoserve: switching to %s (%s %dx%d @%d fps)",
                              elish_cam_name(d->id), d->dev, d->out_w, d->out_h, d->fps);
                    cam = elish_cam_open(d->id, d->req_w, d->req_h, d->fps);
                    if (cam) {
                        const elish_cam_info *info = elish_cam_info_get(cam);
                        aec = aec_init(info);
                        if (elish_cam_start(cam) < 0) {
                            elish_cam_close(cam);
                            cam = NULL;
                        }
                    }
                    if (!cam)
                        ELISH_ERR("autoserve: %s activation failed", elish_cam_name(d->id));
                    frames_since_switch = 0;
                    /* first autofocus sweep ~2 s after switching (frames flowing) */
                    last_af = elish_now_ns() - 18ull * 1000000000ull;
                } else {
                    ELISH_LOG("autoserve: no consumers, camera idle");
                }
                active = cam ? cand : -1;
            }

            /* Display orientation changed?  Rebuilding the camera streams is
             * only safe when nobody is streaming (apps like GNOME Snapshot do
             * not survive a mid-stream format change), so remember it and
             * apply it as soon as the cameras go idle. */
            if (now - last_orient > 4000ull * 1000 * 1000) {
                last_orient = now;
                cur_rebuild_mode = rebuild_mode();
                int t = display_transform();
                if (t != cur_transform) {
                    if (cur_rebuild_mode == 2) {
                        ELISH_LOG("autoserve: display transform %d -> %d, rebuild disabled",
                                  cur_transform, t);
                    } else {
                        pending_transform = t;
                        int idle = active < 0 && users[0] == 0 && users[1] == 0 &&
                                   pwact[0] == 0 && pwact[1] == 0;
                        ELISH_LOG("autoserve: display transform %d -> %d, rebuild %s",
                                  cur_transform, t,
                                  (cur_rebuild_mode == 1 || idle) ? "now" : "deferred until idle");
                    }
                }
            }
            int idle = active < 0 && users[0] == 0 && users[1] == 0 &&
                       pwact[0] == 0 && pwact[1] == 0;
            if (pending_transform >= 0 && (cur_rebuild_mode == 1 || idle)) {
                int t = pending_transform;
                int was_consumed = active >= 0 || users[0] > 0 || users[1] > 0 ||
                                   pwact[0] > 0 || pwact[1] > 0;
                ELISH_LOG("autoserve: %s, rebuilding streams for display transform %d",
                          cur_rebuild_mode == 1 ? "live rebuild" : "idle", t);
                if (cam) {
                    elish_cam_stop(cam);
                    elish_cam_close(cam);
                    cam = NULL;
                }
                for (int i = 0; i < 2; i++) {
                    if (pw[i].cam) {
                        elish_pwcam_destroy(pw[i].cam);
                        pw[i].cam = NULL;
                    }
                    struct as_dev *d = &devs[i];
                    if (d->lb) {
                        elish_lb_close(d->lb);
                        d->lb = NULL;
                    }
                    int rot = (d->id == ELISH_CAM_FRONT && o->frot >= 0) ? o->frot
                            : (d->id == ELISH_CAM_REAR && o->rrot >= 0) ? o->rrot
                            : default_rotation(d->id, t);
                    int ow = d->req_w, oh = d->req_h;
                    int sensor_fps = mode_fps(d->id, d->req_w, d->req_h);
                    if (d->fps <= 0 || d->fps > sensor_fps)
                        d->fps = sensor_fps;
                    if (rot == 90 || rot == 270) {
                        int tmp = ow;
                        ow = oh;
                        oh = tmp;
                    }
                    d->rot = rot;
                    d->out_w = ow;
                    d->out_h = oh;
                    d->lb = elish_lb_open(d->dev, ow, oh, d->fps);
                    if (!d->lb)
                        ELISH_ERR("autoserve: rebuild: cannot reopen loopback %s", d->dev);
                }
                for (int i = 0; i < 2; i++) {
                    struct as_dev *d = &devs[i];
                    char name[64], desc[96];
                    snprintf(name, sizeof(name), "elish-camera-%s",
                             d->id == ELISH_CAM_FRONT ? "front" : "rear");
                    snprintf(desc, sizeof(desc), "Elish %s Camera",
                             d->id == ELISH_CAM_FRONT ? "Front" : "Rear");
                    pw[i].cam = elish_pwcam_create(name, desc,
                                                   d->id == ELISH_CAM_FRONT ? "front" : "back",
                                                   d->out_w, d->out_h, d->fps);
                    if (!pw[i].cam)
                        ELISH_ERR("autoserve: rebuild: cannot publish %s", name);
                }
                free(nv12);
                nv12 = NULL;
                nv12_size = 0;
                cur_transform = t;
                pending_transform = -1;
                active = -1;
                cand = -1;
                cand_count = 0;
                last_users[0] = last_users[1] = 0;
                last_req[0] = last_req[1] = 0;
                last_scan = 0;
                ELISH_LOG("autoserve: rebuild complete (front %dx%d rot %d, rear %dx%d rot %d)",
                          devs[0].out_w, devs[0].out_h, devs[0].rot,
                          devs[1].out_w, devs[1].out_h, devs[1].rot);
                if (cur_rebuild_mode == 1 && was_consumed && snapshot_running())
                    restart_snapshot();
            }
        }

        if (!cam || active < 0) {
            usleep(120 * 1000);
            continue;
        }

        elish_raw_image img;
        uint64_t ts = 0;
        if (elish_cam_get_frame(cam, &img, &ts) < 0) {
            ELISH_ERR("autoserve: capture failed, reconfiguring");
            elish_cam_stop(cam);
            elish_cam_close(cam);
            cam = NULL;
            active = -1;
            usleep(300 * 1000);
            continue;
        }
        const elish_cam_info *info = elish_cam_info_get(cam);
        struct as_dev *d = &devs[active];
        if (frames_since_switch % 8 == 0) {
            elish_stats st;
            if (elish_gpu_stats(gpu, &img, info->bayer, aec.wb, &st) == 0)
                aec_update(&aec, &st, cam);
        }
        size_t need = (size_t)d->out_w * d->out_h * 3 / 2;
        if (nv12_size < need) {
            free(nv12);
            nv12 = malloc(need);
            nv12_size = nv12 ? need : 0;
        }
        if (nv12 && elish_gpu_debayer_nv12(gpu, &img, info->bayer, d->out_w, d->out_h, d->rot,
                                           aec.wb, 1.0f / 2.2f, nv12, d->out_w) == 0) {
            if (d->lb && proc_device_users(d->dev) > 0)
                elish_lb_push_nv12(d->lb, nv12, d->out_w);
            if (elish_pwcam_active(pw[active].cam))
                elish_pwcam_push(pw[active].cam, nv12, d->out_w);
        }
        elish_cam_put_frame(cam);
        if (o->autofocus && d->id == ELISH_CAM_REAR &&
            elish_now_ns() - last_af > 20ull * 1000000000ull) {
            last_af = elish_now_ns();
            double sc = 0;
            int pos = af_sweep(cam, gpu, info, d->out_w, d->out_h, d->rot, aec.wb, g_focus_pos,
                               160, 5, &sc);
            ELISH_LOG("autoserve: autofocus focus=%d score=%.1f", pos, sc);
        }
        frames_since_switch++;
        if (frames_since_switch % 120 == 0)
            ELISH_LOG("autoserve: %s %d frames, wb=(%.2f,1.00,%.2f)", elish_cam_name(d->id),
                      frames_since_switch, aec.wb.r, aec.wb.b);
    }

    if (cam) {
        elish_cam_stop(cam);
        elish_cam_close(cam);
    }
    for (int i = 0; i < 2; i++) {
        if (pw[i].cam)
            elish_pwcam_destroy(pw[i].cam);
        if (devs[i].lb)
            elish_lb_close(devs[i].lb);
    }
    free(nv12);
    elish_gpu_destroy(gpu);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (!strcmp(argv[1], "list"))
        return cmd_list();

    struct opts o;
    if (parse_args(argc - 1, argv + 1, &o) < 0) {
        usage();
        return 1;
    }

    if (!strcmp(argv[1], "photo")) {
        elish_gpu *gpu = elish_gpu_create();
        if (!gpu) {
            ELISH_ERR("GPU init failed");
            return 1;
        }
        aec_t aec;
        elish_camera *cam = camera_ready(&o, &aec, gpu, 16);
        if (!cam) {
            ELISH_ERR("camera open failed");
            elish_gpu_destroy(gpu);
            return 1;
        }
        int rc = write_photo(gpu, cam, &o, &aec);
        elish_cam_stop(cam);
        elish_cam_close(cam);
        elish_gpu_destroy(gpu);
        return rc == 0 ? 0 : 1;
    }
    if (!strcmp(argv[1], "record"))
        return run_output(&o, true, o.loopback != NULL);
    if (!strcmp(argv[1], "stream"))
        return run_output(&o, false, true);
    if (!strcmp(argv[1], "serve"))
        return cmd_serve(&o);
    if (!strcmp(argv[1], "focus"))
        return cmd_focus(&o);
    if (!strcmp(argv[1], "autoserve"))
        return cmd_autoserve(&o);

    usage();
    return 1;
}
