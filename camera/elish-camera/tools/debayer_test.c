/*
 * elish-camera: offline test/benchmark tool for the GPU debayer module.
 *
 * Reads a packed MIPI RAW10 test frame (4 pixels / 5 bytes, see
 * docs/architecture.md section 6), runs the full GPU pipeline through the public
 * elish_gpu_* API and can write PPM (RGB path), raw NV12 and a PPM converted
 * from NV12.  Prints cold/warm/mean/p50/p95 per-frame times and throughput.
 *
 * Examples:
 *   build/debayer_test --input 3280x2464 --stride 4112 --bayer grbg \
 *       --rotate 180 --out-ppm front.ppm --out-nv12 front.nv12
 *   build/debayer_test --input 4208x3120 --stride 5264 --bayer grbg \
 *       --rotate 270 --out-ppm rear.ppm --out-nv12-ppm rear-nv12.ppm --stats
 *   build/debayer_test --input 3280x2464 --stride 4112 --verify 60
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "elish_camera.h"

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

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s RAW_FILE --input WxH --stride S [options]\n"
            "  --file FILE                  raw input file (or first positional argument)\n"
            "  --input WxH                  RAW sensor dimensions\n"
            "  --stride S                   RAW row stride in bytes\n"
            "  --bayer grbg|rggb|bggr|gbrg   Bayer pattern of the RAW data (default grbg)\n"
            "  --rotate 0|90|180|270        clockwise rotation (default 0)\n"
            "  --out-ppm FILE               write RGB888 path as PPM (P6) image\n"
            "  --out-nv12 FILE              write raw NV12 (Y plane then interleaved UV)\n"
            "  --out-nv12-ppm FILE          write PPM converted from the NV12 output\n"
            "  --frames N                   timed frames (default 30)\n"
            "  --warmup N                   warm-up frames before timing (default 8)\n"
            "  --wb r,g,b                   white balance gains (default 1,1,1)\n"
            "  --gamma G                    gamma exponent (default 1/2.2)\n"
            "  --nv12-stride S              caller NV12 row stride in bytes (default out_w)\n"
            "  --stats                      print scene statistics (linear, 0..255)\n"
            "  --verify [N]                 coherency/stability check, N frames (default 60)\n"
            "  --no-timing                  skip the timing loop (useful with --out-*)\n",
            argv0);
}

static int parse_wxh(const char *s, int *w, int *h)
{
    if (sscanf(s, "%dx%d", w, h) != 2 || *w <= 0 || *h <= 0)
        return -1;
    return 0;
}

static int parse_bayer(const char *s, elish_bayer *b)
{
    if (!strcasecmp(s, "rggb"))
        *b = ELISH_BAYER_RGGB;
    else if (!strcasecmp(s, "grbg"))
        *b = ELISH_BAYER_GRBG;
    else if (!strcasecmp(s, "gbrg"))
        *b = ELISH_BAYER_GBRG;
    else if (!strcasecmp(s, "bggr"))
        *b = ELISH_BAYER_BGGR;
    else
        return -1;
    return 0;
}

static uint8_t *read_raw_file(const char *path, int stride, int height)
{
    size_t need = (size_t)stride * (size_t)height;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "debayer_test: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz < 0 || (size_t)sz < need) {
        fprintf(stderr, "debayer_test: %s is %ld bytes, need at least %zu (%dx%d stride %d)\n",
                path, sz, need, stride, height, stride);
        fclose(f);
        return NULL;
    }
    if ((size_t)sz > need)
        fprintf(stderr, "debayer_test: note: %s is %ld bytes, using first %zu\n", path, sz, need);
    uint8_t *buf = malloc(need);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, need, f) != need) {
        fprintf(stderr, "debayer_test: short read from %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

static int write_ppm(const char *path, int w, int h, const uint8_t *rgb, int rgb_stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "debayer_test: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; ++y) {
        if (fwrite(rgb + (size_t)y * (size_t)rgb_stride, 1, (size_t)w * 3, f) != (size_t)w * 3) {
            fprintf(stderr, "debayer_test: write failed for %s\n", path);
            fclose(f);
            return -1;
        }
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

static int write_nv12(const char *path, const uint8_t *nv12, int stride, int out_w, int out_h)
{
    size_t bytes = (size_t)stride * (size_t)out_h * 3 / 2;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "debayer_test: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fwrite(nv12, 1, bytes, f) != bytes) {
        fprintf(stderr, "debayer_test: write failed for %s\n", path);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

/* BT.601 limited range YCbCr -> gamma-encoded RGB, for eyeballing NV12 output */
static int write_ppm_from_nv12(const char *path, const uint8_t *nv12, int stride, int out_w,
                               int out_h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "debayer_test: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", out_w, out_h);
    const uint8_t *uv = nv12 + (size_t)stride * (size_t)out_h;
    uint8_t *row = malloc((size_t)out_w * 3);
    if (!row) {
        fclose(f);
        return -1;
    }
    for (int y = 0; y < out_h; ++y) {
        const uint8_t *yrow = nv12 + (size_t)y * (size_t)stride;
        const uint8_t *uvrow = uv + (size_t)(y / 2) * (size_t)stride;
        for (int x = 0; x < out_w; ++x) {
            double yy = ((double)yrow[x] - 16.0) / 219.0;
            double cb = ((double)uvrow[(x / 2) * 2 + 0] - 128.0) / 224.0;
            double cr = ((double)uvrow[(x / 2) * 2 + 1] - 128.0) / 224.0;
            double r = yy + 1.402 * cr;
            double g = yy - 0.344136 * cb - 0.714136 * cr;
            double b = yy + 1.772 * cb;
            int ri = (int)lrint(fmin(fmax(r, 0.0), 1.0) * 255.0);
            int gi = (int)lrint(fmin(fmax(g, 0.0), 1.0) * 255.0);
            int bi = (int)lrint(fmin(fmax(b, 0.0), 1.0) * 255.0);
            row[x * 3 + 0] = (uint8_t)ri;
            row[x * 3 + 1] = (uint8_t)gi;
            row[x * 3 + 2] = (uint8_t)bi;
        }
        if (fwrite(row, 1, (size_t)out_w * 3, f) != (size_t)out_w * 3) {
            free(row);
            fclose(f);
            return -1;
        }
    }
    free(row);
    return fclose(f) == 0 ? 0 : -1;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

typedef struct {
    int out_w, out_h;
    int nv12_stride;
    size_t nv12_bytes;
} layout_t;

static void layout_init(layout_t *l, int out_w, int out_h, int nv12_stride)
{
    l->out_w = out_w;
    l->out_h = out_h;
    l->nv12_stride = nv12_stride;
    l->nv12_bytes = (size_t)nv12_stride * (size_t)out_h * 3 / 2;
}

static int run_timing(elish_gpu *gpu, const elish_raw_image *raw, elish_bayer bayer,
                      const layout_t *lay, int rotation, elish_wb wb, float gamma, uint8_t *nv12,
                      int frames, int warmup, double *mean_ms_out, double *gbps_out)
{
    int rc = 0;
    uint64_t first_ns = 0;
    uint64_t warm_ns = 0;
    if (elish_gpu_debayer_nv12(gpu, raw, bayer, lay->out_w, lay->out_h, rotation, wb, gamma, nv12,
                               lay->nv12_stride) < 0)
        return -1;

    /* cold frame (also warms GPU clocks) */
    uint64_t t0 = now_ns();
    rc = elish_gpu_debayer_nv12(gpu, raw, bayer, lay->out_w, lay->out_h, rotation, wb, gamma, nv12,
                                lay->nv12_stride);
    first_ns = now_ns() - t0;
    if (rc < 0)
        return -1;

    for (int i = 0; i < warmup; ++i) {
        t0 = now_ns();
        rc = elish_gpu_debayer_nv12(gpu, raw, bayer, lay->out_w, lay->out_h, rotation, wb, gamma,
                                    nv12, lay->nv12_stride);
        warm_ns += now_ns() - t0;
        if (rc < 0)
            return -1;
    }
    if (frames < 1)
        frames = 1;
    double *t = malloc(sizeof(double) * (size_t)frames);
    if (!t)
        return -1;
    for (int i = 0; i < frames; ++i) {
        t0 = now_ns();
        rc = elish_gpu_debayer_nv12(gpu, raw, bayer, lay->out_w, lay->out_h, rotation, wb, gamma,
                                    nv12, lay->nv12_stride);
        t[i] = ns_to_ms(now_ns() - t0);
        if (rc < 0) {
            free(t);
            return -1;
        }
    }
    double sum = 0.0;
    for (int i = 0; i < frames; ++i)
        sum += t[i];
    double mean = sum / frames;
    qsort(t, (size_t)frames, sizeof(double), cmp_double);
    double p50 = t[frames / 2];
    double p95 = t[(int)((double)frames * 0.95) < frames ? (int)((double)frames * 0.95)
                                                        : frames - 1];
    size_t in_bytes = (size_t)raw->stride * (size_t)raw->height;
    *mean_ms_out = mean;
    *gbps_out = (double)(in_bytes + lay->nv12_bytes) / (mean * 1e6);
    printf("timing nv12 %dx%d rot=%d: cold %.2f ms, warm-avg %.2f ms, mean %.2f ms, "
           "p50 %.2f ms, p95 %.2f ms, min %.2f ms, max %.2f ms, %.2f GB/s\n",
           lay->out_w, lay->out_h, rotation, ns_to_ms(first_ns),
           warmup ? ns_to_ms(warm_ns) / warmup : 0.0, mean, p50, p95, t[0], t[frames - 1],
           *gbps_out);
    free(t);
    return 0;
}

/*
 * Coherency / stability check requested by the Lead: the GPU writes into cached
 * dma-buf mappings; stale CPU cache lines would show up as a frame whose content
 * does not match the input.  Phase 1 runs N identical frames (checksum must be
 * constant), phase 2 alternates two different input images every frame and
 * checks that every output matches the expected checksum for that input.
 */
static int run_verify(elish_gpu *gpu, const elish_raw_image *raw, elish_bayer bayer,
                      const layout_t *lay, int rotation, elish_wb wb, float gamma, uint8_t *nv12,
                      int frames)
{
    size_t raw_bytes = (size_t)raw->stride * (size_t)raw->height;
    uint8_t *alt = malloc(raw_bytes);
    if (!alt)
        return -1;
    memcpy(alt, raw->data, raw_bytes);
    for (size_t i = 0; i < raw_bytes; ++i)
        alt[i] ^= 0x10; /* still a valid RAW10 stream, clearly different image */

    elish_raw_image raw_b = *raw;
    raw_b.data = alt;

    printf("verify: %d identical frames + %d alternating frames\n", frames, frames * 2);

    uint64_t ref_a = 0;
    int phase1_bad = 0;
    for (int i = 0; i < frames; ++i) {
        if (elish_gpu_debayer_nv12(gpu, raw, bayer, lay->out_w, lay->out_h, rotation, wb, gamma,
                                   nv12, lay->nv12_stride) < 0) {
            free(alt);
            return -1;
        }
        uint64_t h = fnv1a(nv12, lay->nv12_bytes);
        if (i == 0)
            ref_a = h;
        else if (h != ref_a)
            ++phase1_bad;
    }
    printf("verify: phase 1 (identical input) %s (%d/%d mismatched checksums)\n",
           phase1_bad ? "FAIL" : "PASS", phase1_bad, frames - 1);

    uint64_t ref_b = 0;
    int phase2_bad = 0;
    int distinct = 0;
    for (int i = 0; i < frames * 2; ++i) {
        const elish_raw_image *src = (i & 1) ? &raw_b : raw;
        if (elish_gpu_debayer_nv12(gpu, src, bayer, lay->out_w, lay->out_h, rotation, wb, gamma,
                                   nv12, lay->nv12_stride) < 0) {
            free(alt);
            return -1;
        }
        uint64_t h = fnv1a(nv12, lay->nv12_bytes);
        if (i == 0)
            ref_a = h;
        else if (i == 1) {
            ref_b = h;
            if (ref_b == ref_a) {
                printf("verify: FAIL - alternating images produced identical output\n");
                free(alt);
                return -1;
            }
        } else if ((i & 1) ? (h != ref_b) : (h != ref_a)) {
            ++phase2_bad;
        }
        if (i >= 1 && h != ref_a)
            ++distinct;
    }
    printf("verify: phase 2 (alternating input) %s (%d/%d mismatched checksums)\n",
           phase2_bad ? "FAIL" : "PASS", phase2_bad, frames * 2 - 2);

    /* statistics must track the input too */
    elish_stats sa, sb;
    int ok = 1;
    if (elish_gpu_stats(gpu, raw, bayer, wb, &sa) < 0 ||
        elish_gpu_stats(gpu, &raw_b, bayer, wb, &sb) < 0) {
        free(alt);
        return -1;
    }
    printf("verify: stats A r=%.2f g=%.2f b=%.2f y=%.2f | B r=%.2f g=%.2f b=%.2f y=%.2f\n",
           sa.mean_r, sa.mean_g, sa.mean_b, sa.mean_y, sb.mean_r, sb.mean_g, sb.mean_b, sb.mean_y);
    {
        float dr = fabsf(sa.mean_r - sb.mean_r);
        float dg = fabsf(sa.mean_g - sb.mean_g);
        float db = fabsf(sa.mean_b - sb.mean_b);
        float dmax = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
        if (dmax < 0.2f) {
            printf("verify: WARN - stats barely changed for clearly different input (max d=%.3f)\n",
                   dmax);
            ok = 0;
        }
    }
    free(alt);
    if (phase1_bad || phase2_bad)
        return -1;
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *out_ppm = NULL;
    const char *out_nv12 = NULL;
    const char *out_nv12_ppm = NULL;
    int req_w = 0, req_h = 0, stride = 0, rotation = 0;
    elish_bayer bayer = ELISH_BAYER_GRBG;
    int frames = 30, warmup = 8;
    int do_stats = 0, do_timing = 1, do_verify = 0, verify_frames = 60;
    elish_wb wb = {1.0f, 1.0f, 1.0f};
    float gamma = 1.0f / 2.2f;
    int nv12_stride_opt = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--file") && v) {
            file = v;
            i++;
        } else if (!strcmp(a, "--input") && v) {
            if (parse_wxh(v, &req_w, &req_h) < 0) {
                fprintf(stderr, "debayer_test: bad --input %s\n", v);
                return 2;
            }
            i++;
        } else if (!strcmp(a, "--stride") && v) {
            stride = atoi(v);
            i++;
        } else if (!strcmp(a, "--bayer") && v) {
            if (parse_bayer(v, &bayer) < 0) {
                fprintf(stderr, "debayer_test: bad --bayer %s\n", v);
                return 2;
            }
            i++;
        } else if (!strcmp(a, "--rotate") && v) {
            rotation = atoi(v);
            i++;
        } else if (!strcmp(a, "--out-ppm") && v) {
            out_ppm = v;
            i++;
        } else if (!strcmp(a, "--out-nv12") && v) {
            out_nv12 = v;
            i++;
        } else if (!strcmp(a, "--out-nv12-ppm") && v) {
            out_nv12_ppm = v;
            i++;
        } else if (!strcmp(a, "--frames") && v) {
            frames = atoi(v);
            i++;
        } else if (!strcmp(a, "--warmup") && v) {
            warmup = atoi(v);
            i++;
        } else if (!strcmp(a, "--wb") && v) {
            if (sscanf(v, "%f,%f,%f", &wb.r, &wb.g, &wb.b) != 3) {
                fprintf(stderr, "debayer_test: bad --wb %s\n", v);
                return 2;
            }
            i++;
        } else if (!strcmp(a, "--gamma") && v) {
            gamma = strtof(v, NULL);
            i++;
        } else if (!strcmp(a, "--nv12-stride") && v) {
            nv12_stride_opt = atoi(v);
            i++;
        } else if (!strcmp(a, "--stats")) {
            do_stats = 1;
        } else if (!strcmp(a, "--no-timing")) {
            do_timing = 0;
        } else if (!strcmp(a, "--verify")) {
            do_verify = 1;
            if (v && v[0] >= '0' && v[0] <= '9') {
                verify_frames = atoi(v);
                i++;
            }
        } else if (a[0] != '-') {
            file = a;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "debayer_test: unknown option %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (!file || !req_w || !req_h || stride <= 0) {
        usage(argv[0]);
        return 2;
    }
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        fprintf(stderr, "debayer_test: bad --rotate %d\n", rotation);
        return 2;
    }

    uint8_t *raw_data = read_raw_file(file, stride, req_h);
    if (!raw_data)
        return 1;

    elish_raw_image raw = {.data = raw_data, .width = req_w, .height = req_h, .stride = stride};

    int out_w = req_w, out_h = req_h;
    if (rotation == 90 || rotation == 270) {
        out_w = req_h;
        out_h = req_w;
    }
    if (out_w & 1 || out_h & 1) {
        fprintf(stderr, "debayer_test: odd output dimensions are not supported for NV12\n");
        free(raw_data);
        return 1;
    }
    layout_t lay;
    layout_init(&lay, out_w, out_h, nv12_stride_opt > 0 ? nv12_stride_opt : out_w);
    if (lay.nv12_stride < out_w) {
        fprintf(stderr, "debayer_test: --nv12-stride %d < out_w %d\n", lay.nv12_stride, out_w);
        free(raw_data);
        return 1;
    }

    elish_gpu *gpu = elish_gpu_create();
    if (!gpu) {
        fprintf(stderr, "debayer_test: elish_gpu_create failed\n");
        free(raw_data);
        return 1;
    }
    printf("GPU renderer: %s\n", elish_gpu_renderer(gpu));
    printf("input %s %dx%d stride %d bayer %d, output %dx%d rot=%d nv12_stride=%d wb=(%.2f,%.2f,"
           "%.2f) gamma=%.4f\n",
           file, req_w, req_h, stride, (int)bayer, out_w, out_h, rotation, lay.nv12_stride, wb.r,
           wb.g, wb.b, gamma);

    uint8_t *nv12 = malloc(lay.nv12_bytes);
    if (!nv12) {
        elish_gpu_destroy(gpu);
        free(raw_data);
        return 1;
    }

    int rc = 0;
    if (do_verify) {
        rc = run_verify(gpu, &raw, bayer, &lay, rotation, wb, gamma, nv12, verify_frames);
        if (rc < 0) {
            fprintf(stderr, "debayer_test: verify FAILED\n");
            goto out;
        }
        printf("verify: ALL PASS\n");
    }

    if (do_timing) {
        double mean_ms = 0.0, gbps = 0.0;
        if (run_timing(gpu, &raw, bayer, &lay, rotation, wb, gamma, nv12, frames, warmup, &mean_ms,
                       &gbps) < 0) {
            fprintf(stderr, "debayer_test: NV12 pipeline failed\n");
            rc = 1;
            goto out;
        }
    } else if (elish_gpu_debayer_nv12(gpu, &raw, bayer, out_w, out_h, rotation, wb, gamma, nv12,
                                      lay.nv12_stride) < 0) {
        fprintf(stderr, "debayer_test: NV12 pipeline failed\n");
        rc = 1;
        goto out;
    }

    if (do_stats) {
        elish_stats st;
        uint64_t t0 = now_ns();
        if (elish_gpu_stats(gpu, &raw, bayer, wb, &st) < 0) {
            fprintf(stderr, "debayer_test: stats failed\n");
            rc = 1;
            goto out;
        }
        double cold_ms = ns_to_ms(now_ns() - t0);
        /* first call includes lazy shader compilation; time warm calls too */
        int stats_runs = frames > 3 ? frames : 3;
        double stats_sum = 0.0, stats_max = 0.0;
        for (int i = 0; i < stats_runs; ++i) {
            t0 = now_ns();
            if (elish_gpu_stats(gpu, &raw, bayer, wb, &st) < 0) {
                fprintf(stderr, "debayer_test: stats failed\n");
                rc = 1;
                goto out;
            }
            double ms = ns_to_ms(now_ns() - t0);
            stats_sum += ms;
            if (ms > stats_max)
                stats_max = ms;
        }
        printf("stats (linear pre-gamma, black-level subtracted, 0..255): r=%.3f g=%.3f b=%.3f "
               "y=%.3f (cold %.2f ms, warm mean %.2f ms, max %.2f ms, R/G=%.4f G/B=%.4f)\n",
               st.mean_r, st.mean_g, st.mean_b, st.mean_y, cold_ms, stats_sum / stats_runs,
               stats_max, st.mean_g > 0 ? st.mean_r / st.mean_g : 0.0f,
               st.mean_g > 0 ? st.mean_g / st.mean_b : 0.0f);
    }

    if (out_nv12 && write_nv12(out_nv12, nv12, lay.nv12_stride, out_w, out_h) < 0)
        rc = 1;
    else if (out_nv12)
        printf("wrote NV12: %s (%dx%d stride %d, %zu bytes)\n", out_nv12, out_w, out_h,
               lay.nv12_stride, lay.nv12_bytes);
    if (out_nv12_ppm && write_ppm_from_nv12(out_nv12_ppm, nv12, lay.nv12_stride, out_w, out_h) < 0)
        rc = 1;
    else if (out_nv12_ppm)
        printf("wrote PPM from NV12: %s\n", out_nv12_ppm);

    if (out_ppm) {
        uint8_t *rgb = malloc((size_t)out_w * (size_t)out_h * 3);
        if (!rgb) {
            rc = 1;
            goto out;
        }
        uint64_t t0 = now_ns();
        int r = elish_gpu_debayer_rgb(gpu, &raw, bayer, out_w, out_h, rotation, wb, gamma, rgb,
                                      out_w * 3);
        double first_ms = ns_to_ms(now_ns() - t0);
        if (r < 0) {
            fprintf(stderr, "debayer_test: RGB pipeline failed\n");
            free(rgb);
            rc = 1;
            goto out;
        }
        double rsum = 0.0;
        int rgb_frames = frames > 0 ? frames : 1;
        for (int i = 0; i < rgb_frames; ++i) {
            t0 = now_ns();
            if (elish_gpu_debayer_rgb(gpu, &raw, bayer, out_w, out_h, rotation, wb, gamma, rgb,
                                      out_w * 3) < 0) {
                free(rgb);
                rc = 1;
                goto out;
            }
            rsum += ns_to_ms(now_ns() - t0);
        }
        printf("timing rgb888 %dx%d rot=%d: cold %.2f ms, mean %.2f ms\n", out_w, out_h, rotation,
               first_ms, rsum / rgb_frames);
        if (write_ppm(out_ppm, out_w, out_h, rgb, out_w * 3) < 0)
            rc = 1;
        else
            printf("wrote PPM: %s\n", out_ppm);
        free(rgb);
    }

out:
    free(nv12);
    elish_gpu_destroy(gpu);
    free(raw_data);
    return rc;
}
