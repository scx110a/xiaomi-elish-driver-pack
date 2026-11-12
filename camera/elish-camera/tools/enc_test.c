/*
 * enc_test - offline throughput / correctness test for the Venus hardware
 * encoder (src/venus_enc.c).
 *
 * Feeds synthetic or real-sensor NV12 frames to the hardware encoder through
 * the public elish_enc_* API and reports per-frame latency, sustained
 * throughput, bitrate, keyframe structure and CPU usage.
 *
 * Frame sources:
 *   --input FILE           headerless NV12 (Y plane then interleaved UV,
 *                          stride = width), first frame is looped
 *   --raw10 FILE           packed MIPI RAW10 GRBG frame (testdata/<camera>.raw,
 *                          docs/architecture.md §6); converted to NV12 with a
 *                          cheap 2x2 demosaic + auto contrast
 *   --synthetic PATTERN    gradient | noise | bars | black (default gradient)
 *
 * Motion: --scroll N shifts the real frame N pixels per frame (wrap around)
 * so P frames carry a realistic residual; synthetic patterns are inherently
 * animated (noise is regenerated per frame).
 *
 * Links against build/venus_enc.o only (no other elish-camera module).
 */

#include <elish_camera.h>

#include "../src/venus_enc_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define ET_MAX_FRAMES 1000000

enum src_kind {
    SRC_SYNTHETIC = 0,
    SRC_NV12_FILE,
    SRC_RAW10,
};

struct opts {
    elish_codec codec;
    int width;
    int height;
    int fps;
    int bitrate;
    int frames;
    int keyint;
    int force_idr;
    int scroll;
    bool realtime;
    bool verbose;
    const char *input;
    const char *raw10;
    const char *raw10_stride_set;
    const char *synthetic;
    const char *out;
    const char *mux;
};

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double ns_to_ms(uint64_t ns)
{
    return (double)ns / 1e6;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;

    return (va > vb) - (va < vb);
}

static void usage(const char *argv0)
{
    printf(
        "usage: %s --size WxH [options]\n"
        "\n"
        "  --codec h264|hevc     codec (default hevc)\n"
        "  --size WxH            picture size, required (e.g. 4208x3120)\n"
        "  --fps N               nominal frame rate (default 30)\n"
        "  --bitrate BPS         target bitrate in bits/s (default 20000000)\n"
        "  --frames N            number of frames (default 90)\n"
        "  --keyint N            driver GOP size (default: one second)\n"
        "  --force-idr N         force an IDR every N frames via the explicit\n"
        "                        force-keyframe control (default 0 = off)\n"
        "  --input FILE          raw NV12 frame (looped)\n"
        "  --raw10 FILE          packed MIPI RAW10 GRBG frame (testdata)\n"
        "  --raw10-stride N      row stride of the RAW10 file (default:\n"
        "                        16-byte aligned width*5/4)\n"
        "  --synthetic PATTERN   gradient|noise|bars|black (default gradient)\n"
        "  --scroll N            shift real frames N px/frame (default 2)\n"
        "  --realtime            pace pushes at --fps instead of running flat out\n"
        "  --out FILE            elementary stream output\n"
        "  --mux FILE            also stream-copy the ES into an MP4 with ffmpeg\n"
        "  --verbose             print every frame (default: every 10th)\n"
        "  -h|--help\n",
        argv0);
}

#define NEED_ARG()                                                             \
    do {                                                                       \
        if (i + 1 >= argc) {                                                   \
            fprintf(stderr, "enc_test: %s needs a value\n", argv[i]);          \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static int parse_args(int argc, char **argv, struct opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->codec = ELISH_CODEC_HEVC;
    o->fps = 30;
    o->bitrate = 20000000;
    o->frames = 90;
    o->scroll = -1;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(a, "--codec")) {
            NEED_ARG();
            i++;
            if (!strcmp(argv[i], "h264"))
                o->codec = ELISH_CODEC_H264;
            else if (!strcmp(argv[i], "hevc"))
                o->codec = ELISH_CODEC_HEVC;
            else {
                fprintf(stderr, "enc_test: unknown codec '%s'\n", argv[i]);
                return -1;
            }
        } else if (!strcmp(a, "--size")) {
            int w, h, n;

            NEED_ARG();
            i++;
            n = sscanf(argv[i], "%dx%d", &w, &h);
            if (n != 2 || w <= 0 || h <= 0) {
                fprintf(stderr, "enc_test: bad --size '%s' (want WxH)\n",
                        argv[i]);
                return -1;
            }
            o->width = w;
            o->height = h;
        } else if (!strcmp(a, "--fps")) {
            NEED_ARG();
            o->fps = atoi(argv[++i]);
        } else if (!strcmp(a, "--bitrate")) {
            NEED_ARG();
            o->bitrate = atoi(argv[++i]);
        } else if (!strcmp(a, "--frames")) {
            NEED_ARG();
            o->frames = atoi(argv[++i]);
        } else if (!strcmp(a, "--keyint")) {
            NEED_ARG();
            o->keyint = atoi(argv[++i]);
        } else if (!strcmp(a, "--force-idr")) {
            NEED_ARG();
            o->force_idr = atoi(argv[++i]);
        } else if (!strcmp(a, "--input")) {
            NEED_ARG();
            o->input = argv[++i];
        } else if (!strcmp(a, "--raw10")) {
            NEED_ARG();
            o->raw10 = argv[++i];
        } else if (!strcmp(a, "--raw10-stride")) {
            NEED_ARG();
            o->raw10_stride_set = argv[++i];
        } else if (!strcmp(a, "--synthetic")) {
            NEED_ARG();
            o->synthetic = argv[++i];
        } else if (!strcmp(a, "--scroll")) {
            NEED_ARG();
            o->scroll = atoi(argv[++i]);
        } else if (!strcmp(a, "--realtime")) {
            o->realtime = true;
        } else if (!strcmp(a, "--out")) {
            NEED_ARG();
            o->out = argv[++i];
        } else if (!strcmp(a, "--mux")) {
            NEED_ARG();
            o->mux = argv[++i];
        } else if (!strcmp(a, "--verbose")) {
            o->verbose = true;
        } else {
            fprintf(stderr, "enc_test: unknown option '%s' (try --help)\n", a);
            return -1;
        }
    }

    if (o->width <= 0 || o->height <= 0) {
        fprintf(stderr, "enc_test: --size WxH is required\n");
        return -1;
    }
    if ((o->width & 1) || (o->height & 1)) {
        fprintf(stderr, "enc_test: NV12/NV12 source needs even dimensions\n");
        return -1;
    }
    if (o->fps <= 0 || o->fps > 960) {
        fprintf(stderr, "enc_test: invalid --fps %d\n", o->fps);
        return -1;
    }
    if (o->frames <= 0 || o->frames > ET_MAX_FRAMES) {
        fprintf(stderr, "enc_test: invalid --frames %d\n", o->frames);
        return -1;
    }
    if (o->input && o->raw10) {
        fprintf(stderr, "enc_test: --input and --raw10 are mutually exclusive\n");
        return -1;
    }
    if (o->scroll < 0)
        o->scroll = (o->input || o->raw10) ? 2 : 0;
    if (o->scroll > 0 && o->width > 0 && o->scroll >= o->width)
        o->scroll %= o->width;
    return 0;
}

#undef NEED_ARG

/* ------------------------------------------------------------------ */
/* synthetic frames                                                   */
/* ------------------------------------------------------------------ */

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void gen_synthetic(uint8_t *nv12, int w, int h, const char *pattern,
                          int frame)
{
    uint8_t *uv = nv12 + (size_t)w * h;
    uint32_t seed = 0x12345678u * (uint32_t)(frame + 1) + 0x9e3779b9u;
    int x, y;

    if (!strcmp(pattern, "black")) {
        memset(nv12, 16, (size_t)w * h);
        memset(uv, 128, (size_t)w * h / 2);
        return;
    }
    if (!strcmp(pattern, "noise")) {
        for (y = 0; y < h; y++) {
            uint8_t *row = nv12 + (size_t)y * w;

            for (x = 0; x < w; x++)
                row[x] = (uint8_t)(xs32(&seed) >> 24);
        }
        for (y = 0; y < h / 2; y++) {
            uint8_t *row = uv + (size_t)y * w;

            for (x = 0; x < w; x++)
                row[x] = (uint8_t)(xs32(&seed) >> 24);
        }
        return;
    }
    if (!strcmp(pattern, "bars")) {
        int shift = (frame * 4) % (w > 0 ? w : 1);

        for (y = 0; y < h; y++) {
            uint8_t *row = nv12 + (size_t)y * w;

            for (x = 0; x < w; x++) {
                int bar = (((x + shift) * 8) / w) & 7;

                row[x] = (uint8_t)(16 + bar * 30);
            }
        }
        for (y = 0; y < h / 2; y++) {
            uint8_t *row = uv + (size_t)y * w;

            for (x = 0; x < w; x += 2) {
                int bar = (((x + shift) * 8) / w) & 7;

                row[x] = (uint8_t)(100 + bar * 3);
                row[x + 1] = (uint8_t)(150 - bar * 3);
            }
        }
        return;
    }

    /* default: gradient with a moving bright block, animated per frame */
    for (y = 0; y < h; y++) {
        uint8_t *row = nv12 + (size_t)y * w;

        for (x = 0; x < w; x++) {
            int v = (x * 200) / w + (y * 40) / h + frame * 3;

            row[x] = (uint8_t)(16 + (v & 0xff) * 200 / 255);
        }
    }
    {
        int bx = (frame * 8) % (w > 0 ? w : 1);
        int by = h / 3;

        for (y = by; y < by + h / 8 && y < h; y++) {
            uint8_t *row = nv12 + (size_t)y * w;

            for (x = bx; x < bx + w / 8 && x < w; x++)
                row[x] = 235;
        }
    }
    for (y = 0; y < h / 2; y++) {
        uint8_t *row = uv + (size_t)y * w;

        for (x = 0; x < w; x += 2) {
            row[x] = (uint8_t)(128 + (x - w / 2) * 60 / (w ? w : 1));
            row[x + 1] = (uint8_t)(128 + (y - h / 4) * 60 / (h ? h : 1));
        }
    }
}

/* ------------------------------------------------------------------ */
/* packed MIPI RAW10 (GRBG) -> NV12                                   */
/* ------------------------------------------------------------------ */

static int raw10_percentile(const uint32_t *hist, uint64_t total, int pct)
{
    uint64_t target = total * (uint64_t)pct / 1000;
    uint64_t cum = 0;
    int i;

    for (i = 0; i < 1024; i++) {
        cum += hist[i];
        if (cum >= target)
            return i;
    }
    return 1023;
}

static int load_raw10_nv12(const char *path, int w, int h, int stride,
                           uint8_t *nv12)
{
    FILE *fp;
    uint8_t *rows;
    uint16_t *row0, *row1;
    uint32_t hist[1024];
    uint64_t total = 0;
    uint8_t lut[1024];
    int lo, hi;
    int nbytes = w * 5 / 4;
    int x, y, g;

    if (w % 4 != 0) {
        ELISH_ERR("enc_test: RAW10 requires width divisible by 4, got %d", w);
        return -1;
    }
    if (h % 2 != 0) {
        ELISH_ERR("enc_test: RAW10 requires even height, got %d", h);
        return -1;
    }
    if (stride < nbytes) {
        ELISH_ERR("enc_test: RAW10 stride %d < row bytes %d", stride, nbytes);
        return -1;
    }
    rows = malloc((size_t)stride * 2);
    row0 = malloc((size_t)w * sizeof(uint16_t));
    row1 = malloc((size_t)w * sizeof(uint16_t));
    if (!rows || !row0 || !row1) {
        free(rows);
        free(row0);
        free(row1);
        return -1;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        ELISH_ERR("enc_test: open %s: %s", path, strerror(errno));
        free(rows);
        free(row0);
        free(row1);
        return -1;
    }

    memset(hist, 0, sizeof(hist));

    for (y = 0; y < h; y += 2) {
        int r;

        for (r = 0; r < 2; r++) {
            uint16_t *dst = (r == 0) ? row0 : row1;

            if (fread(rows, 1, (size_t)stride, fp) != (size_t)stride) {
                ELISH_ERR("enc_test: %s: short read at row %d", path, y + r);
                goto fail;
            }
            for (g = 0; g < nbytes / 5; g++) {
                const uint8_t *b = rows + g * 5;

                dst[g * 4 + 0] =
                    (uint16_t)(((uint16_t)b[0] << 2) | ((b[4] >> 0) & 3));
                dst[g * 4 + 1] =
                    (uint16_t)(((uint16_t)b[1] << 2) | ((b[4] >> 2) & 3));
                dst[g * 4 + 2] =
                    (uint16_t)(((uint16_t)b[2] << 2) | ((b[4] >> 4) & 3));
                dst[g * 4 + 3] =
                    (uint16_t)(((uint16_t)b[3] << 2) | ((b[4] >> 6) & 3));
            }
        }

        /* GRBG: (0,0)=G (0,1)=R (1,0)=B (1,1)=G.  The greens are the best
         * exposure reference, so build the contrast histogram from them. */
        for (x = 0; x < w; x += 2) {
            hist[row0[x]]++;
            hist[row1[x + 1]]++;
        }
    }
    fclose(fp);
    fp = NULL;

    for (x = 0; x < 1024; x++)
        total += hist[x];
    lo = raw10_percentile(hist, total, 5);   /* 0.5 % */
    hi = raw10_percentile(hist, total, 995); /* 99.5 % */
    if (hi <= lo + 32) {
        lo = 0;
        hi = 1023;
    }
    for (x = 0; x < 1024; x++) {
        int v = (x - lo) * 255 / (hi - lo);

        lut[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    /* Second pass: decode again and convert 8-bit GRBG -> BT.601 limited
     * range NV12.  Each pixel uses its own channel for luma (full resolution
     * detail) and the 2x2 cell for chroma. */
    fp = fopen(path, "rb");
    if (!fp) {
        ELISH_ERR("enc_test: reopen %s: %s", path, strerror(errno));
        free(rows);
        free(row0);
        free(row1);
        return -1;
    }
    for (y = 0; y < h; y += 2) {
        int r;

        for (r = 0; r < 2; r++) {
            uint16_t *dst = (r == 0) ? row0 : row1;

            if (fread(rows, 1, (size_t)stride, fp) != (size_t)stride) {
                ELISH_ERR("enc_test: %s: short read at row %d (pass 2)", path,
                          y + r);
                goto fail;
            }
            for (g = 0; g < nbytes / 5; g++) {
                const uint8_t *b = rows + g * 5;

                dst[g * 4 + 0] =
                    (uint16_t)(((uint16_t)b[0] << 2) | ((b[4] >> 0) & 3));
                dst[g * 4 + 1] =
                    (uint16_t)(((uint16_t)b[1] << 2) | ((b[4] >> 2) & 3));
                dst[g * 4 + 2] =
                    (uint16_t)(((uint16_t)b[2] << 2) | ((b[4] >> 4) & 3));
                dst[g * 4 + 3] =
                    (uint16_t)(((uint16_t)b[3] << 2) | ((b[4] >> 6) & 3));
            }
        }

        for (x = 0; x < w; x += 2) {
            int R = lut[row0[x + 1]];
            int G0 = lut[row0[x]];
            int G1 = lut[row1[x + 1]];
            int B = lut[row1[x]];
            int gavg = (G0 + G1) / 2;
            int Y00, Y01, Y10, Y11, U, V;
            uint8_t *yp0 = nv12 + (size_t)y * w;
            uint8_t *yp1 = nv12 + (size_t)(y + 1) * w;
            uint8_t *uv = nv12 + (size_t)w * h + (size_t)(y / 2) * w;

            Y00 = (((66 * R + 129 * G0 + 25 * B + 128) >> 8) + 16);
            Y01 = (((66 * R + 129 * gavg + 25 * B + 128) >> 8) + 16);
            Y10 = Y01;
            Y11 = (((66 * R + 129 * G1 + 25 * B + 128) >> 8) + 16);
            U = (((-38 * R - 74 * gavg + 112 * B + 128) >> 8) + 128);
            V = (((112 * R - 94 * gavg - 18 * B + 128) >> 8) + 128);

            yp0[x] = (uint8_t)(Y00 < 16 ? 16 : (Y00 > 235 ? 235 : Y00));
            yp0[x + 1] = (uint8_t)(Y01 < 16 ? 16 : (Y01 > 235 ? 235 : Y01));
            yp1[x] = (uint8_t)(Y10 < 16 ? 16 : (Y10 > 235 ? 235 : Y10));
            yp1[x + 1] = (uint8_t)(Y11 < 16 ? 16 : (Y11 > 235 ? 235 : Y11));
            uv[x] = (uint8_t)(U < 16 ? 16 : (U > 240 ? 240 : U));
            uv[x + 1] = (uint8_t)(V < 16 ? 16 : (V > 240 ? 240 : V));
        }
    }

    ELISH_LOG("enc_test: RAW10 %s: BT.601 limited range, contrast lo=%d hi=%d",
              path, lo, hi);
    free(rows);
    free(row0);
    free(row1);
    return 0;

fail:
    if (fp)
        fclose(fp);
    free(rows);
    free(row0);
    free(row1);
    return -1;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct opts o;
    struct elish_encoder *enc;
    struct rusage ru0, ru1;
    uint8_t *src = NULL;
    uint8_t *frame = NULL;
    uint8_t *scratch = NULL;
    uint64_t *lat = NULL;
    enum src_kind kind;
    const char *synth = "gradient";
    const char *out;
    char out_default[128];
    char fps_buf[16];
    struct elish_enc_stats st;
    int rc;
    int i;
    int push_fail = 0;
    uint64_t t_start, t_push_end, t_stop_end, t_pace = 0;
    uint64_t late_max_ns = 0;
    int late_frames = 0;
    size_t frame_bytes;
    double cpu_s, wall_s, video_s;

    if (parse_args(argc, argv, &o) < 0)
        return 2;

    if (o.raw10)
        kind = SRC_RAW10;
    else if (o.input)
        kind = SRC_NV12_FILE;
    else {
        kind = SRC_SYNTHETIC;
        if (o.synthetic)
            synth = o.synthetic;
        if (strcmp(synth, "gradient") && strcmp(synth, "noise") &&
            strcmp(synth, "bars") && strcmp(synth, "black")) {
            fprintf(stderr, "enc_test: unknown synthetic pattern '%s'\n", synth);
            return 2;
        }
    }

    frame_bytes = (size_t)o.width * (size_t)o.height * 3 / 2;
    frame = malloc(frame_bytes);
    if (kind != SRC_SYNTHETIC) {
        src = malloc(frame_bytes);
        scratch = malloc(frame_bytes);
    }
    lat = calloc((size_t)o.frames, sizeof(uint64_t));
    if (!frame || (kind != SRC_SYNTHETIC && (!src || !scratch)) || !lat) {
        fprintf(stderr, "enc_test: out of memory\n");
        return 2;
    }

    if (kind == SRC_NV12_FILE) {
        FILE *fp = fopen(o.input, "rb");
        size_t got;

        if (!fp) {
            fprintf(stderr, "enc_test: open %s: %s\n", o.input, strerror(errno));
            return 2;
        }
        got = fread(src, 1, frame_bytes, fp);
        fclose(fp);
        if (got < frame_bytes) {
            fprintf(stderr,
                    "enc_test: %s has %zu bytes, need %zu for %dx%d NV12\n",
                    o.input, got, frame_bytes, o.width, o.height);
            return 2;
        }
    } else if (kind == SRC_RAW10) {
        int stride = o.raw10_stride_set
                         ? atoi(o.raw10_stride_set)
                         : ((o.width * 5 / 4 + 15) & ~15);

        if (load_raw10_nv12(o.raw10, o.width, o.height, stride, src) < 0)
            return 2;
    }

    if (o.out) {
        out = o.out;
    } else {
        snprintf(out_default, sizeof(out_default), "/tmp/enc_test_%dx%d.%s",
                 o.width, o.height,
                 o.codec == ELISH_CODEC_H264 ? "h264" : "h265");
        out = out_default;
    }
    unlink(out);

    if (o.keyint > 0) {
        char buf[32];

        snprintf(buf, sizeof(buf), "%d", o.keyint);
        setenv("ELISH_ENC_GOP", buf, 1);
    }

    enc = elish_enc_create(o.codec, o.width, o.height, o.fps, o.bitrate, out);
    if (!enc) {
        fprintf(stderr, "enc_test: elish_enc_create failed\n");
        return 1;
    }

    printf("== elish enc_test ==\n");
    printf("codec      : %s\n", o.codec == ELISH_CODEC_H264 ? "h264" : "hevc");
    printf("size       : %dx%d\n", o.width, o.height);
    printf("fps        : %d\n", o.fps);
    printf("bitrate    : %d bit/s (%.2f Mbit/s)\n", o.bitrate,
           o.bitrate / 1e6);
    printf("frames     : %d\n", o.frames);
    printf("keyint     : %d (driver GOP), force-idr every %d\n", o.keyint,
           o.force_idr);
    printf("source     : %s\n",
           kind == SRC_SYNTHETIC
               ? synth
               : (kind == SRC_RAW10 ? o.raw10 : o.input));
    if (o.scroll)
        printf("motion     : scroll %d px/frame\n", o.scroll);
    printf("output     : %s\n", out);
    printf("pacing     : %s\n", o.realtime ? "realtime" : "max throughput");
    printf("\n");
    fflush(stdout);

    if (elish_enc_start(enc) < 0) {
        fprintf(stderr, "enc_test: elish_enc_start failed\n");
        elish_enc_destroy(enc);
        return 1;
    }

    getrusage(RUSAGE_SELF, &ru0);
    t_start = now_ns();

    for (i = 0; i < o.frames; i++) {
        const uint8_t *push_src;
        uint64_t pts = (uint64_t)i * 1000000000ull / (uint64_t)o.fps;
        uint64_t t0, t1, target = 0;
        bool force = (i == 0) || (o.force_idr > 0 && (i % o.force_idr) == 0);

        /* The first push pays the one-off Venus firmware boot (~0.8 s), so
         * realtime pacing starts once it has returned. */
        if (o.realtime && i > 0) {
            target = t_pace + (uint64_t)(i - 1) * 1000000000ull /
                                  (uint64_t)o.fps;
            while (now_ns() < target) {
                uint64_t left = target - now_ns();
                struct timespec ts = {
                    .tv_sec = (time_t)(left / 1000000000ull),
                    .tv_nsec = (long)(left % 1000000000ull),
                };

                nanosleep(&ts, NULL);
            }
        }

        if (kind == SRC_SYNTHETIC) {
            gen_synthetic(frame, o.width, o.height, synth, i);
            push_src = frame;
        } else if (o.scroll > 0) {
            int dx = (int)(((uint64_t)i * (uint64_t)o.scroll) %
                           (uint64_t)o.width);
            int p;

            for (p = 0; p < 2; p++) {
                int rows = (p == 0) ? o.height : o.height / 2;
                size_t off = (p == 0) ? 0 : (size_t)o.width * o.height;
                int y;

                for (y = 0; y < rows; y++) {
                    const uint8_t *sr = src + off + (size_t)y * o.width;
                    uint8_t *dr = scratch + off + (size_t)y * o.width;

                    memcpy(dr, sr + dx, (size_t)(o.width - dx));
                    memcpy(dr + (o.width - dx), sr, (size_t)dx);
                }
            }
            push_src = scratch;
        } else {
            push_src = src;
        }

        t0 = now_ns();
        if (elish_enc_push_nv12(enc, push_src, o.width, pts, force) < 0) {
            fprintf(stderr, "enc_test: push failed at frame %d\n", i);
            push_fail++;
            break;
        }
        t1 = now_ns();
        lat[i] = t1 - t0;
        if (i == 0)
            t_pace = t1;

        if (o.realtime && i > 0 && t1 > target) {
            uint64_t late = t1 - target;

            if (late > late_max_ns)
                late_max_ns = late;
            if (late > (uint64_t)(1000000000ull / (uint64_t)o.fps))
                late_frames++;
        }

        if (o.verbose || (i % 10) == 0 || i == o.frames - 1) {
            printf("frame %4d  push %7.2f ms  cum %8.1f ms%s\n", i,
                   ns_to_ms(lat[i]), ns_to_ms(t1 - t_start),
                   force ? "  (forced IDR)" : "");
            fflush(stdout);
        }
    }
    t_push_end = now_ns();

    if (elish_enc_stop(enc) < 0) {
        fprintf(stderr, "enc_test: elish_enc_stop reported errors\n");
        push_fail++;
    }
    t_stop_end = now_ns();
    getrusage(RUSAGE_SELF, &ru1);

    elish_enc_get_stats(enc, &st);

    wall_s = (double)(t_stop_end - t_start) / 1e9;
    video_s = (double)(st.frames_encoded > 0 ? st.frames_encoded : (uint64_t)i) /
              (double)o.fps;
    cpu_s = (double)(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) +
            (double)(ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e6 +
            (double)(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) +
            (double)(ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;

    {
        int n = i;
        int m = (n > 1) ? n - 1 : 0; /* steady-state frames (skip warm-up) */
        int k;
        uint64_t sum = 0, p95 = 0, maxlat = 0, minlat = 0;

        if (m > 0) {
            uint64_t *sorted = malloc((size_t)m * sizeof(uint64_t));

            minlat = lat[1];
            for (k = 1; k < n; k++) {
                sum += lat[k];
                if (lat[k] > maxlat)
                    maxlat = lat[k];
                if (lat[k] < minlat)
                    minlat = lat[k];
            }
            if (sorted) {
                memcpy(sorted, &lat[1], (size_t)m * sizeof(uint64_t));
                qsort(sorted, (size_t)m, sizeof(uint64_t), cmp_u64);
                p95 = sorted[(size_t)(m * 95 / 100)];
                free(sorted);
            }
        }

        printf("\n--- summary ---\n");
        printf("enc frame      : %dx%d (padded %dx%d, stride %d)\n", st.width,
               st.height, st.in_width, st.in_height, st.stride);
        printf("frames pushed  : %d/%d\n", i, o.frames);
        printf("frames encoded : %llu\n", (unsigned long long)st.frames_encoded);
        printf("encode errors  : %d\n", push_fail);
        printf("frames dropped : %s\n",
               (st.frames_encoded == (uint64_t)o.frames && st.skipped == 0 &&
                push_fail == 0)
                   ? "0"
                   : "yes");
        printf("firmware skips : %llu (empty capture buffers)\n",
               (unsigned long long)st.skipped);
        printf("keyframes (V4L2 flag): %llu\n", (unsigned long long)st.keyframes);
        printf("es bytes       : %llu (%.2f MiB)\n", (unsigned long long)st.bytes,
               st.bytes / 1024.0 / 1024.0);
        printf("video duration : %.3f s (%llu frames @ %d fps)\n", video_s,
               (unsigned long long)st.frames_encoded, o.fps);
        printf("wall time      : %.3f s (push %.3f s + flush %.3f s)\n", wall_s,
               (double)(t_push_end - t_start) / 1e9,
               (double)(t_stop_end - t_push_end) / 1e9);
        printf("startup        : first frame took %.1f ms (firmware boot)\n",
               n > 0 ? ns_to_ms(lat[0]) : 0.0);
        printf("throughput     : %.2f fps end-to-end, %.2f fps steady-state\n",
               n / wall_s,
               (n > 1 && t_stop_end > t_pace)
                   ? (double)(n - 1) / ((double)(t_stop_end - t_pace) / 1e9)
                   : 0.0);
        if (m > 0) {
            printf("push latency   : min %.2f / avg %.2f / p95 %.2f / max %.2f ms "
                   "(steady-state, first frame excluded)\n",
                   ns_to_ms(minlat), ns_to_ms(sum / (uint64_t)m),
                   ns_to_ms(p95), ns_to_ms(maxlat));
        }
        printf("cpu            : %.2f s user+sys -> %.1f %% of one core\n", cpu_s,
               wall_s > 0 ? cpu_s / wall_s * 100.0 : 0.0);
        printf("bitrate        : %.2f Mbit/s (target %.2f, %+.1f %%) (ES bytes / "
               "video duration)\n",
               video_s > 0 ? (double)st.bytes * 8.0 / video_s / 1e6 : 0.0,
               o.bitrate / 1e6,
               (video_s > 0 && o.bitrate > 0)
                   ? (((double)st.bytes * 8.0 / video_s / (double)o.bitrate) - 1.0) *
                         100.0
                   : 0.0);
        if (o.realtime && m > 0) {
            printf("realtime       : %d frame(s) late by more than one frame "
                   "period, worst lateness %.2f ms\n",
                   late_frames, ns_to_ms(late_max_ns));
        }
    }

    if (o.mux) {
        snprintf(fps_buf, sizeof(fps_buf), "%d", o.fps);
        setenv("ELISH_MUX_FPS", fps_buf, 1);
        printf("\nmuxing %s -> %s ...\n", out, o.mux);
        fflush(stdout);
        if (elish_mux_mp4(out, o.mux, o.codec) < 0)
            fprintf(stderr, "enc_test: mux failed\n");
        else
            printf("mux ok: %s\n", o.mux);
    }

    rc = (i == o.frames && push_fail == 0) ? 0 : 1;
    printf("\nresult: %s\n", rc == 0 ? "PASS" : "FAIL");

    elish_enc_destroy(enc);
    free(src);
    free(frame);
    free(scratch);
    free(lat);
    return rc;
}
