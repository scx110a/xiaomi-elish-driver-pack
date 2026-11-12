/*
 * loopback_test.c - push NV12 frames into a v4l2loopback virtual camera.
 *
 * Feeds elish-camera's loopback producer (src/loopback.c) and measures the
 * actually achieved frame rate.  Frame sources:
 *
 *   --pattern bars|checker|gradient   synthetic frame (default: bars) with
 *                                    an optional moving marker (--static to
 *                                    disable) so consumers show live video
 *   --input file.nv12                raw NV12 file (Y plane then UV plane),
 *                                    stride inferred from the file size
 *   --raw file.raw                   packed MIPI RAW10 capture from testdata/
 *                                    (e.g. testdata/front-3280x2464-sgrbg10.raw),
 *                                    converted to NV12 on the CPU once
 *
 * Typical use:
 *   build/loopback_test --dev /dev/video2 --size 1920x1080 --fps 30 --seconds 10
 *   build/loopback_test --raw testdata/front-3280x2464-sgrbg10.raw \
 *                       --size 3280x2464 --fps 30 --seconds 10
 *
 * Always run it under the loopback flock (see README.md):
 *   flock -w 60 elish-camera/.locks/loopback.lock -c './build/loopback_test ...'
 */
#include "elish_camera.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* clock / stats                                                      */
/* ------------------------------------------------------------------ */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* synthetic frame generation                                         */
/* ------------------------------------------------------------------ */

enum pattern { PAT_BARS, PAT_CHECKER, PAT_GRADIENT };

static void fill_pattern(uint8_t *nv12, int w, int h, enum pattern pat)
{
    uint8_t *y = nv12;
    uint8_t *uv = nv12 + (size_t)w * h;
    /* SMPTE-ish colours, limited range BT.601 (Y, U, V). */
    static const uint8_t bars[8][3] = {
        { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
        { 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 },
    };

    if (pat == PAT_CHECKER) {
        int sq = h / 8;
        if (sq < 1)
            sq = 1;
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                int on = ((xx / sq) + (yy / sq)) & 1;
                y[(size_t)yy * w + xx] = on ? 220 : 20;
            }
        }
        for (int yy = 0; yy < h / 2; yy++)
            memset(uv + (size_t)yy * w, 128, (size_t)w);
    } else if (pat == PAT_GRADIENT) {
        for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++)
                y[(size_t)yy * w + xx] = (uint8_t)((xx * 255 / (w > 1 ? w - 1 : 1)) & 0xff);
        for (int yy = 0; yy < h / 2; yy++)
            for (int xx = 0; xx < w; xx += 2) {
                uv[(size_t)yy * w + xx] = (uint8_t)((yy * 255 / (h > 2 ? h / 2 - 1 : 1)) & 0xff);
                uv[(size_t)yy * w + xx + 1] = (uint8_t)((xx * 255 / (w > 1 ? w - 1 : 1)) & 0xff);
            }
    } else { /* colour bars */
        for (int yy = 0; yy < h; yy++) {
            for (int xx = 0; xx < w; xx++) {
                int idx = xx * 8 / w;
                if (idx > 7)
                    idx = 7;
                y[(size_t)yy * w + xx] = bars[idx][0];
            }
        }
        for (int yy = 0; yy < h / 2; yy++) {
            for (int xx = 0; xx < w; xx += 2) {
                int idx = xx * 8 / w;
                if (idx > 7)
                    idx = 7;
                uv[(size_t)yy * w + xx] = bars[idx][1];
                uv[(size_t)yy * w + xx + 1] = bars[idx][2];
            }
        }
        /* White top strip so orientation is obvious in a GUI app. */
        int strip = h / 24 + 1;
        for (int yy = 0; yy < strip && yy < h; yy++)
            memset(y + (size_t)yy * w, 235, (size_t)w);
    }
}

/* Draw a filled moving box; restores the previous box from the saved patch
 * so the underlying pattern is preserved. */
typedef struct {
    int w, h;
    int bw, bh;
    int x, y;
    bool valid;
} marker;

static int marker_init(marker *m, int w, int h)
{
    memset(m, 0, sizeof(*m));
    m->w = w;
    m->h = h;
    m->bw = w / 8;
    m->bh = h / 8;
    if (m->bw < 8)
        m->bw = 8;
    if (m->bh < 8)
        m->bh = 8;
    return 0;
}

static inline uint8_t *px(uint8_t *nv12, int w, int h, int x, int y)
{
    return nv12 + (size_t)y * w + x;
}

/* Put the pristine background back where the marker was. */
static void marker_restore(marker *m, const uint8_t *tmpl, uint8_t *nv12)
{
    const uint8_t *ty = tmpl;
    const uint8_t *tuv = tmpl + (size_t)m->w * m->h;
    uint8_t *y = nv12;
    uint8_t *uv = nv12 + (size_t)m->w * m->h;

    if (!m->valid)
        return;
    for (int r = 0; r < m->bh; r++)
        memcpy(y + (size_t)(m->y + r) * m->w + m->x,
               ty + (size_t)(m->y + r) * m->w + m->x, (size_t)m->bw);
    for (int r = 0; r < m->bh / 2; r++)
        memcpy(uv + (size_t)((m->y / 2) + r) * m->w + (m->x & ~1),
               tuv + (size_t)((m->y / 2) + r) * m->w + (m->x & ~1), (size_t)m->bw);
}

static void marker_draw(marker *m, uint8_t *nv12, int64_t t)
{
    uint8_t *y = nv12;
    uint8_t *uv = nv12 + (size_t)m->w * m->h;
    int span_x = m->w - m->bw;
    int span_y = m->h - m->bh;

    if (span_x < 1)
        span_x = 1;
    if (span_y < 1)
        span_y = 1;
    m->x = (int)((t * (span_x / 2)) % span_x);
    m->y = (int)((t * (span_y / 3)) % span_y);
    m->x &= ~1;
    m->y &= ~1;

    m->valid = true;

    /* solid orange box, Y/Cb/Cr = 180/80/190 */
    for (int r = 0; r < m->bh; r++)
        memset(y + (size_t)(m->y + r) * m->w + m->x, 180, (size_t)m->bw);
    for (int r = 0; r < m->bh / 2; r++) {
        for (int c = 0; c < m->bw; c += 2) {
            px(uv, m->w, m->h / 2, (m->x & ~1) + c, (m->y / 2) + r)[0] = 80;
            px(uv, m->w, m->h / 2, (m->x & ~1) + c + 1, (m->y / 2) + r)[0] = 190;
        }
    }
}

/* ------------------------------------------------------------------ */
/* MIPI RAW10 -> NV12 (simple CPU nearest-neighbour demosaic)         */
/* ------------------------------------------------------------------ */

static inline int raw10_get(const uint8_t *raw, int stride, int x, int y)
{
    size_t off = (size_t)y * stride + (size_t)(x >> 2) * 5;
    int hi = raw[off + (x & 3)];
    int lo = (raw[off + 4] >> (2 * (x & 3))) & 3;
    return (hi << 2) | lo;
}

typedef enum {
    BAYER_RGGB = 0,
    BAYER_GRBG = 1,
    BAYER_GBRG = 2,
    BAYER_BGGR = 3,
} raw_bayer;

static int raw10_to_nv12(const uint8_t *raw, int raw_w, int raw_h, int stride,
                         raw_bayer bayer, int out_w, int out_h, uint8_t *nv12)
{
    uint8_t *y = nv12;
    uint8_t *uv = nv12 + (size_t)out_w * out_h;
    int ox, oy;

    if (raw_w < 2 || raw_h < 2 || out_w < 2 || out_h < 2)
        return -1;

    for (oy = 0; oy < out_h; oy++) {
        int sy = (int)((int64_t)oy * raw_h / out_h);
        sy &= ~1;
        if (sy > raw_h - 2)
            sy = raw_h - 2;
        for (ox = 0; ox < out_w; ox++) {
            int sx = (int)((int64_t)ox * raw_w / out_w);
            sx &= ~1;
            if (sx > raw_w - 2)
                sx = raw_w - 2;

            int p00 = raw10_get(raw, stride, sx, sy);
            int p01 = raw10_get(raw, stride, sx + 1, sy);
            int p10 = raw10_get(raw, stride, sx, sy + 1);
            int p11 = raw10_get(raw, stride, sx + 1, sy + 1);
            int r, g, b;

            switch (bayer) {
            case BAYER_RGGB: /* R G / G B */
                r = p00; g = (p01 + p10) / 2; b = p11;
                break;
            case BAYER_GRBG: /* G R / B G */
                r = p01; g = (p00 + p11) / 2; b = p10;
                break;
            case BAYER_GBRG: /* G B / R G */
                r = p10; g = (p00 + p11) / 2; b = p01;
                break;
            default: /* BGGR: B G / G R */
                r = p11; g = (p01 + p10) / 2; b = p00;
                break;
            }
            /* 10-bit -> 8-bit, BT.601 limited range luma. */
            int yy = (299 * (r >> 2) + 587 * (g >> 2) + 114 * (b >> 2)) / 1000;
            if (yy < 16)
                yy = 16;
            if (yy > 235)
                yy = 235;
            y[(size_t)oy * out_w + ox] = (uint8_t)yy;

            if ((ox & 1) == 0) {
                int cb = 128 + (int)((b >> 2) - yy) * 564 / 1000;
                int cr = 128 + (int)((r >> 2) - yy) * 713 / 1000;
                if (cb < 16) cb = 16;
                if (cb > 240) cb = 240;
                if (cr < 16) cr = 16;
                if (cr > 240) cr = 240;
                size_t uvidx = (size_t)(oy / 2) * out_w + (size_t)(ox & ~1);
                uv[uvidx] = (uint8_t)cb;
                uv[uvidx + 1] = (uint8_t)cr;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* file loading                                                       */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;

    if (!f) {
        ELISH_ERR("cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        ELISH_ERR("cannot determine size of %s", path);
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)sz);
    if (!buf) {
        ELISH_ERR("out of memory reading %s (%ld bytes)", path, sz);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        ELISH_ERR("short read on %s", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)sz;
    return buf;
}

static int parse_size(const char *s, int *w, int *h)
{
    char *end = NULL;
    long a, b;
    if (!s)
        return -1;
    a = strtol(s, &end, 10);
    if (!end || (*end != 'x' && *end != 'X'))
        return -1;
    b = strtol(end + 1, &end, 10);
    if (!end || *end != '\0' || a <= 0 || b <= 0 || a > 65536 || b > 65536)
        return -1;
    *w = (int)a;
    *h = (int)b;
    return 0;
}

static raw_bayer parse_bayer(const char *s)
{
    if (!s)
        return BAYER_GRBG;
    if (!strcasecmp(s, "rggb"))
        return BAYER_RGGB;
    if (!strcasecmp(s, "grbg"))
        return BAYER_GRBG;
    if (!strcasecmp(s, "gbrg"))
        return BAYER_GBRG;
    if (!strcasecmp(s, "bggr"))
        return BAYER_BGGR;
    ELISH_ERR("unknown bayer pattern '%s' (use rggb/grbg/gbrg/bggr)", s);
    return BAYER_GRBG;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --dev PATH        v4l2loopback device (default /dev/video2)\n"
            "  --size WxH        output size (default 1920x1080)\n"
            "  --fps N           target frame rate (default 30)\n"
            "  --seconds S       run time in seconds (default 10; 0 = forever)\n"
            "  --pattern NAME    bars|checker|gradient (default bars)\n"
            "  --static          do not animate the marker (lower CPU cost)\n"
            "  --input FILE      raw NV12 file to push\n"
            "  --raw FILE        packed MIPI RAW10 file to convert (testdata/*.raw)\n"
            "  --raw-size WxH    raw frame size (default: from --size)\n"
            "  --raw-stride N    raw row stride in bytes (default: file_size / raw_height)\n"
            "  --bayer NAME      rggb|grbg|gbrg|bggr (default grbg)\n"
            "  --stats S         print a stats line every S seconds (default 1)\n"
            "  -h, --help        this help\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/video2";
    const char *input = NULL;
    const char *rawfile = NULL;
    int out_w = 1920, out_h = 1080;
    int raw_w = 0, raw_h = 0, raw_stride = 0;
    int fps = 30;
    double seconds = 10.0;
    double stats_interval = 1.0;
    enum pattern pat = PAT_BARS;
    bool animate = true;
    raw_bayer bayer = BAYER_GRBG;
    uint8_t *frame = NULL;
    uint8_t *template = NULL;
    size_t frame_payload = 0;
    marker mk;
    bool have_marker = false;
    elish_loopback *lb;
    uint64_t start_ns, end_ns;
    uint64_t stats_last_ns, stats_last_count;
    uint64_t pushed = 0;
    int rc = 1;
    int opt;

    static const struct option long_opts[] = {
        { "dev", required_argument, NULL, 'd' },
        { "size", required_argument, NULL, 's' },
        { "fps", required_argument, NULL, 'f' },
        { "seconds", required_argument, NULL, 't' },
        { "pattern", required_argument, NULL, 'p' },
        { "static", no_argument, NULL, 'S' },
        { "input", required_argument, NULL, 'i' },
        { "raw", required_argument, NULL, 'r' },
        { "raw-size", required_argument, NULL, 'R' },
        { "raw-stride", required_argument, NULL, 'T' },
        { "bayer", required_argument, NULL, 'b' },
        { "stats", required_argument, NULL, 'q' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    while ((opt = getopt_long(argc, argv, "hd:s:f:t:p:Si:r:R:T:b:q:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd':
            dev = optarg;
            break;
        case 's':
            if (parse_size(optarg, &out_w, &out_h) < 0) {
                ELISH_ERR("bad --size '%s' (want WxH)", optarg);
                return 2;
            }
            break;
        case 'f':
            fps = atoi(optarg);
            if (fps <= 0) {
                ELISH_ERR("bad --fps '%s'", optarg);
                return 2;
            }
            break;
        case 't':
            seconds = atof(optarg);
            if (seconds < 0) {
                ELISH_ERR("bad --seconds '%s'", optarg);
                return 2;
            }
            break;
        case 'p':
            if (!strcmp(optarg, "bars"))
                pat = PAT_BARS;
            else if (!strcmp(optarg, "checker"))
                pat = PAT_CHECKER;
            else if (!strcmp(optarg, "gradient"))
                pat = PAT_GRADIENT;
            else {
                ELISH_ERR("unknown --pattern '%s'", optarg);
                return 2;
            }
            break;
        case 'S':
            animate = false;
            break;
        case 'i':
            input = optarg;
            break;
        case 'r':
            rawfile = optarg;
            break;
        case 'R':
            if (parse_size(optarg, &raw_w, &raw_h) < 0) {
                ELISH_ERR("bad --raw-size '%s'", optarg);
                return 2;
            }
            break;
        case 'T':
            raw_stride = atoi(optarg);
            break;
        case 'b':
            bayer = parse_bayer(optarg);
            break;
        case 'q':
            stats_interval = atof(optarg);
            if (stats_interval <= 0)
                stats_interval = 1.0;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 2;
        }
    }

    if (out_w % 2 || out_h % 2) {
        ELISH_ERR("NV12 requires even width/height, got %dx%d", out_w, out_h);
        return 2;
    }
    frame_payload = (size_t)out_w * out_h * 3 / 2;
    frame = calloc(1, frame_payload);
    if (!frame) {
        ELISH_ERR("cannot allocate %zu byte frame", frame_payload);
        return 1;
    }

    if (input || rawfile) {
        size_t sz = 0;
        uint8_t *data = read_file(input ? input : rawfile, &sz);
        if (!data)
            goto out;

        if (input) {
            /* NV12 file: either tightly packed (stride == width) or a file
             * whose size encodes a larger stride. UV stride assumed equal. */
            if (sz == frame_payload) {
                memcpy(frame, data, frame_payload);
            } else if (sz % ((size_t)out_h * 3 / 2) == 0) {
                int stride = (int)(sz / ((size_t)out_h * 3 / 2));
                if (stride < out_w || stride > 65536) {
                    ELISH_ERR("--input %s: inferred stride %d unusable", input, stride);
                    free(data);
                    goto out;
                }
                for (int yy = 0; yy < out_h; yy++)
                    memcpy(frame + (size_t)yy * out_w,
                           data + (size_t)yy * stride, (size_t)out_w);
                for (int yy = 0; yy < out_h / 2; yy++)
                    memcpy(frame + (size_t)out_w * out_h + (size_t)yy * out_w,
                           data + (size_t)stride * out_h + (size_t)yy * stride,
                           (size_t)out_w);
                ELISH_LOG("loaded NV12 %s: %dx%d stride %d", input, out_w, out_h, stride);
            } else {
                ELISH_ERR("--input %s: size %zu does not match NV12 %dx%d (%zu bytes)",
                          input, sz, out_w, out_h, frame_payload);
                free(data);
                goto out;
            }
            if (sz == frame_payload)
                ELISH_LOG("loaded NV12 %s: %dx%d tightly packed", input, out_w, out_h);
        } else {
            int rw = raw_w ? raw_w : out_w;
            int rh = raw_h ? raw_h : out_h;
            int rstride = raw_stride;
            if (!rstride) {
                if (rh > 0 && sz % (size_t)rh == 0)
                    rstride = (int)(sz / (size_t)rh);
                else
                    rstride = (rw * 5 + 3) / 4;
            }
            if (rstride < (rw * 5 + 3) / 4 || sz < (size_t)rstride * (size_t)rh) {
                ELISH_ERR("--raw %s: %zu bytes too small for %dx%d stride %d",
                          rawfile, sz, rw, rh, rstride);
                free(data);
                goto out;
            }
            ELISH_LOG("converting RAW10 %s (%dx%d stride %d, bayer %s) -> NV12 %dx%d",
                      rawfile, rw, rh, rstride,
                      bayer == BAYER_GRBG ? "grbg" : bayer == BAYER_RGGB ? "rggb"
                      : bayer == BAYER_GBRG ? "gbrg" : "bggr",
                      out_w, out_h);
            if (raw10_to_nv12(data, rw, rh, rstride, bayer, out_w, out_h, frame) < 0) {
                ELISH_ERR("RAW10 conversion failed");
                free(data);
                goto out;
            }
        }
        free(data);
    } else {
        fill_pattern(frame, out_w, out_h, pat);
        if (animate) {
            /* Keep an untouched copy so the moving marker can be re-drawn
             * on a clean background every frame. */
            template = malloc(frame_payload);
            if (!template) {
                ELISH_ERR("cannot allocate template frame");
                goto out;
            }
            memcpy(template, frame, frame_payload);
            if (marker_init(&mk, out_w, out_h) < 0) {
                ELISH_ERR("cannot allocate marker patch");
                goto out;
            }
            have_marker = true;
        }
        ELISH_LOG("synthetic %s pattern %dx%d%s", pat == PAT_BARS ? "bars"
                  : pat == PAT_CHECKER ? "checker" : "gradient",
                  out_w, out_h, animate ? " (animated marker)" : "");
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, on_signal);

    lb = elish_lb_open(dev, out_w, out_h, fps);
    if (!lb) {
        rc = 1;
        goto out;
    }

    start_ns = now_ns();
    stats_last_ns = start_ns;
    stats_last_count = 0;

    while (!g_stop) {
        uint64_t t = now_ns();
        if (seconds > 0 && (double)(t - start_ns) / 1e9 >= seconds)
            break;

        if (have_marker) {
            marker_restore(&mk, template, frame);
            marker_draw(&mk, frame, (int64_t)((t - start_ns) / 1000000));
        }

        if (elish_lb_push_nv12(lb, frame, out_w) < 0) {
            ELISH_ERR("push failed after %llu frames, aborting",
                      (unsigned long long)pushed);
            rc = 1;
            break;
        }
        pushed++;
        rc = 0;

        t = now_ns();
        if (stats_interval > 0 && (double)(t - stats_last_ns) / 1e9 >= stats_interval) {
            double dt = (double)(t - stats_last_ns) / 1e9;
            double inst = (double)(pushed - stats_last_count) / dt;
            double elapsed = (double)(t - start_ns) / 1e9;
            fprintf(stderr, "[%6.2fs] frames=%llu  inst_fps=%.1f  avg_fps=%.1f\n",
                    elapsed, (unsigned long long)pushed, inst,
                    elapsed > 0 ? (double)pushed / elapsed : 0.0);
            stats_last_ns = t;
            stats_last_count = pushed;
        }
    }

    end_ns = now_ns();
    {
        double elapsed = (double)(end_ns - start_ns) / 1e9;
        double avg = elapsed > 0 ? (double)pushed / elapsed : 0.0;
        if (elapsed <= 0)
            elapsed = 1e-9;
        printf("pushed %llu frames in %.3f s: actual_fps=%.2f (target %d fps, "
               "expected=%llu, %s)\n",
               (unsigned long long)pushed, elapsed, avg, fps,
               (unsigned long long)llround((double)fps * elapsed),
               avg >= fps * 0.97 ? "OK" : "BELOW TARGET");
    }

    elish_lb_close(lb);

out:
    free(frame);
    free(template);
    return rc;
}
