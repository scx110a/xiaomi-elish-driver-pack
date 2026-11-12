/*
 * elish-camera: GPU demosaic + colour pipeline for Xiaomi Pad 5 Pro
 * (SM8250 / Adreno 650 "FD650", Mesa freedreno, EGL/GLES 3.2).
 *
 * Pipeline
 * --------
 *   packed MIPI RAW10 (stride x height bytes)
 *     --[glTexSubImage2D, GL_R8UI]--> packed texture
 *     --[unpack pass]---------------> R16F "raw" texture (W x H, 0..1023)
 *     --[demosaic + WB + gamma]-----> Y  plane (R8, full res)
 *                                     UV plane (GR88, half res)      (NV12)
 *                                  or RGBA8 full res                 (RGB888)
 *     --[scene statistics]----------> 512x512 block means -> 64x64 readback
 *
 * Readback
 * --------
 * The GPU renders into dma-bufs allocated from /dev/dma_heap/system, whose CPU
 * mmap is *cached*: a 13 MB readback copies at ~10 GB/s instead of the
 * ~0.9 GB/s of glReadPixels on this device (freedreno readpixels ends up in a
 * write-combined staging buffer; an 8 MP plane alone cost ~17 ms).  The
 * dma-bufs are imported as EGLImages and bound as renderbuffers, so the GPU
 * renders directly into cached system memory; the API then memcpy()s rows into
 * the caller's NV12/RGB888 buffer.
 *
 * Y and UV live in ONE dma-buf (Y at offset 0, UV at an aligned offset) so a
 * frame needs a single DMA_BUF_IOCTL_SYNC pair, then the API assembles the
 * caller's contiguous NV12 buffer (Y plane followed by interleaved UV, caller
 * stride) with two row-copies.  Cache maintenance between GPU and CPU is done
 * with DMA_BUF_IOCTL_SYNC; see debayer_test --verify (120 alternating frames)
 * which checks that no stale frame content is ever observed.
 *
 * Demosaic
 * --------
 * Bilinear interpolation with proper per-site reconstruction (no 2x2 nearest
 * neighbour copy).  Luma is reconstructed at full resolution; NV12 chroma is
 * the average of the 4x4 RAW neighbourhood of each 2x2 output block (R/B:
 * 4 samples, G: 8 samples), the usual ISP chroma treatment, which keeps the
 * half-resolution chroma plane cheap.
 *
 * Colour
 * ------
 * black level subtract (64 of 1024, OV8856/OV13B10) -> white balance ->
 * normalise -> gamma (caller supplied, typically 1/2.2) -> BT.601 limited
 * range YCbCr for NV12.  The statistics interface reports pre-gamma linear
 * means in 0..255 scale: mean of max(RAW-64,0) / 4 per Bayer colour, with the
 * white balance gains applied to the means, and mean_y is Rec.601 luma of
 * those.
 *
 * Rotation 0/90/180/270 (clockwise) is applied by mapping output pixel
 * coordinates back to RAW coordinates in the shader, so the Bayer pattern is
 * always demosaiced in sensor geometry.
 *
 * Debug: ELISH_GPU_PROFILE=1 prints per-stage timings (upload/unpack/y/uv/
 * copy) for the last call; it inserts glFinish() between stages, so the total
 * is slightly pessimistic but the breakdown is accurate.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "elish_camera.h"

/* ------------------------------------------------------------------ */
/* configuration                                                      */
/* ------------------------------------------------------------------ */

/*
 * Scene statistics: three-pass GPU reduction with FIXED 4x4 kernels.
 *
 * Measured on fd650: a single fragment shader with a dynamic per-block loop
 * (a few thousand fragments, tens to thousands of dependent texelFetch per
 * fragment) is 5-20x slower than the same total work spread over many
 * fragments with short, compile-time-unrolled loops.  Hence:
 *   pass A: up to 1024x1024 fragments, each covering one 8x8 RAW block; only
 *           two rows of the block are sampled (both row parities), i.e. 16 of
 *           64 pixels, which is plenty for a mean and halves the fetch count
 *   pass B: 1024 -> 256 -> 64, each step a fixed 4x4 box reduction, weighted
 *           by coverage so partially valid edge cells are not dropped
 * The RAW region used is a centred crop of at most 8192x8192 pixels, so both
 * sensors are covered in full.  Readback is only the 64x64 result (16 KB).
 */
#define STATS_GRID_A 1024
#define STATS_GRID_B 256
#define STATS_OUT 64
#define MAX_DIM 16384

/*
 * Sensor black level in 10-bit RAW units (both OV8856 and OV13B10 report 64
 * per the Android HAL evidence used by the Lead; adjust here if that changes).
 * RAW values are black-level subtracted before white balance; the display
 * pipeline normalises to 0..1 by dividing by (1023-64), the statistics
 * pipeline reports pre-gamma linear means in 0..255 scale i.e. raw units / 4.
 */
#define RAW_BLACK_LEVEL 64.0f
#define RAW_WHITE (1023.0f - RAW_BLACK_LEVEL)

static uint64_t tick_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define PROF_MARK(g, t0, label)                                                \
    do {                                                                       \
        if ((g)->profile) {                                                    \
            glFinish();                                                        \
            uint64_t _t1 = tick_ns();                                          \
            fprintf(stderr, "elish-gpu: %-8s %7.3f ms\n", label,              \
                    (double)(_t1 - (t0)) / 1e6);                               \
            (t0) = _t1;                                                        \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------ */
/* GPU program + GPU->CPU targets                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    GLuint id;
    GLint u_packed;   /* usampler2D packed RAW10 bytes */
    GLint u_raw;      /* sampler2D  R16F decoded RAW   */
    GLint u_src;      /* sampler2D  stats intermediate */
    GLint u_size;     /* ivec2      RAW dimensions     */
    GLint u_raw_size; /* ivec2      RAW dimensions     */
    GLint u_rot;      /* int        rotation degrees   */
    GLint u_c;        /* ivec4      Bayer colours      */
    GLint u_wb;       /* vec3       white balance      */
    GLint u_gamma;    /* float      gamma              */
    GLint u_cnt;      /* vec3       1/count per colour */
    GLint u_off;      /* ivec2      stats block origin */
    GLint u_grid;     /* int        stats grid         */
} gpu_program;

/* a single plane dma-buf imported as an EGLImage renderbuffer */
typedef struct {
    int fd;
    uint8_t *map;
    size_t size;
    uint32_t pitch;
    int w, h;
    EGLImageKHR img;
    GLuint rb;
} gpu_target;

/* Y+UV in one dma-buf (two EGLImages at different offsets) */
typedef struct {
    int fd;
    uint8_t *map;
    size_t size;
    uint32_t pitch;
    int w, h;
    size_t uv_off;
    EGLImageKHR img_y, img_uv;
    GLuint rb_y, rb_uv;
} gpu_nv12;

struct elish_gpu {
    int drm_fd;
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf; /* only used if surfaceless make-current is unsupported */
    char renderer[64];
    int profile;

    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_target_rb;

    GLuint vao;
    GLuint fbo;

    gpu_program p_unpack;
    gpu_program p_y;
    gpu_program p_uv;
    gpu_program p_rgb;
    gpu_program p_stats;
    gpu_program p_stats_ds;

    /* cached RAW domain resources */
    GLuint tex_pack;
    int pack_stride, pack_h;
    GLuint tex_raw;
    int raw_w, raw_h;

    /* cached NV12 target (Y + UV in one dma-buf) */
    gpu_nv12 nv12;
    int nv12_w, nv12_h;

    /* cached RGB target */
    gpu_target t_rgb;
    int rgb_w, rgb_h;

    /* scene statistics */
    GLuint stats_tex;  /* STATS_GRID_A x STATS_GRID_A RGBA16F render target */
    GLuint stats_fbo;  /* FBO with stats_tex attached */
    GLuint stats_tex2; /* STATS_GRID_B x STATS_GRID_B RGBA16F render target */
    GLuint stats_fbo2;
    GLuint stats_rb; /* STATS_OUT x STATS_OUT RGBA16F readback target */
    GLuint stats_fbo_out;
};

/* ------------------------------------------------------------------ */
/* GLSL                                                               */
/* ------------------------------------------------------------------ */

static const char *VS_FULLSCREEN =
    "#version 300 es\n"
    "void main()\n"
    "{\n"
    "    vec2 p = vec2(gl_VertexID == 0 ? -1.0 : (gl_VertexID == 1 ? 3.0 : -1.0),\n"
    "                  gl_VertexID == 0 ? -1.0 : (gl_VertexID == 2 ? 3.0 : -1.0));\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

/* packed MIPI RAW10: 4 pixels / 5 bytes, p_i = (b_i<<2) | ((b4>>(2i))&3) */
static const char *FS_UNPACK =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp usampler2D u_packed;\n"
    "uniform highp ivec2 u_size;\n"
    "out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    int g = p.x >> 2;\n"
    "    int idx = p.x & 3;\n"
    "    uint hi = texelFetch(u_packed, ivec2(g * 5 + idx, p.y), 0).r;\n"
    "    uint lo = texelFetch(u_packed, ivec2(g * 5 + 4, p.y), 0).r;\n"
    "    uint v = (hi << 2) | ((lo >> (2 * idx)) & 3u);\n"
    "    fragColor = vec4(float(v));\n"
    "}\n";

/* shared demosaic prelude for the luma / chroma / rgb passes */
#define DEMOSAIC_PRELUDE                                                     \
    "#version 300 es\n"                                                      \
    "precision highp float;\n"                                               \
    "precision highp int;\n"                                                 \
    "uniform highp sampler2D u_raw;\n"                                       \
    "uniform highp ivec2 u_raw_size;\n"                                      \
    "uniform highp int u_rot;\n"                                             \
    "uniform highp ivec4 u_c;\n" /* colour at (0,0),(1,0),(0,1),(1,1) */    \
    "uniform highp vec3 u_wb;\n"                                             \
    "uniform highp float u_gamma;\n"                                         \
    "float rawFetch(ivec2 p)\n"                                              \
    "{\n"                                                                    \
    "    p = clamp(p, ivec2(0), u_raw_size - 1);\n"                          \
    "    return texelFetch(u_raw, p, 0).r;\n"                                \
    "}\n"                                                                    \
    "int colorAt(ivec2 p)\n"                                                 \
    "{\n"                                                                    \
    "    bool xe = ((p.x & 1) == 0);\n"                                      \
    "    bool ye = ((p.y & 1) == 0);\n"                                      \
    "    if (ye) return xe ? u_c.x : u_c.y;\n"                               \
    "    return xe ? u_c.z : u_c.w;\n"                                       \
    "}\n"                                                                    \
    "ivec2 mapSrc(ivec2 o)\n"                                                \
    "{\n"                                                                    \
    "    if (u_rot == 0) return o;\n"                                        \
    "    if (u_rot == 90) return ivec2(o.y, u_raw_size.y - 1 - o.x);\n"      \
    "    if (u_rot == 180)\n"                                                \
    "        return ivec2(u_raw_size.x - 1 - o.x, u_raw_size.y - 1 - o.y);\n" \
    "    return ivec2(u_raw_size.x - 1 - o.y, o.x);\n"                       \
    "}\n"

/* bilinear demosaic, RAW units */
#define DEMOSAIC_BILINEAR                                                    \
    "vec3 demosaic(ivec2 p)\n"                                               \
    "{\n"                                                                    \
    "    float C = rawFetch(p);\n"                                           \
    "    float N = rawFetch(p + ivec2(0, -1));\n"                            \
    "    float S = rawFetch(p + ivec2(0, 1));\n"                             \
    "    float E = rawFetch(p + ivec2(1, 0));\n"                             \
    "    float W = rawFetch(p + ivec2(-1, 0));\n"                            \
    "    int c = colorAt(p);\n"                                              \
    "    vec3 rgb;\n"                                                        \
    "    if (c == 1) {\n"                                                    \
    "        int hc = colorAt(p + ivec2(1, 0));\n"                           \
    "        float h = 0.5 * (E + W);\n"                                     \
    "        float v = 0.5 * (N + S);\n"                                     \
    "        rgb.g = C;\n"                                                   \
    "        if (hc == 0) { rgb.r = h; rgb.b = v; }\n"                       \
    "        else         { rgb.b = h; rgb.r = v; }\n"                       \
    "    } else {\n"                                                         \
    "        float ne = rawFetch(p + ivec2(1, -1));\n"                       \
    "        float nw = rawFetch(p + ivec2(-1, -1));\n"                      \
    "        float se = rawFetch(p + ivec2(1, 1));\n"                        \
    "        float sw = rawFetch(p + ivec2(-1, 1));\n"                       \
    "        rgb.g = 0.25 * (N + S + E + W);\n"                              \
    "        if (c == 0) { rgb.r = C; rgb.b = 0.25 * (ne + nw + se + sw); }\n" \
    "        else         { rgb.b = C; rgb.r = 0.25 * (ne + nw + se + sw); }\n" \
    "    }\n"                                                                \
    "    return rgb;\n"                                                      \
    "}\n"

#define COLORIZE                                                           \
    "const float u_black = 64.0;\n" /* sensor black level, 10-bit RAW units */ \
    "const float u_white = 959.0;\n" /* 1023 - 64 */                       \
    "vec3 colorize(vec3 rgb)\n"                                            \
    "{\n"                                                                  \
    "    rgb = max(rgb - u_black, 0.0) / u_white;\n"                       \
    "    rgb = rgb * u_wb;\n"                                              \
    "    rgb = clamp(rgb, 0.0, 1.0);\n"                                    \
    "    return pow(rgb, vec3(u_gamma));\n"                                \
    "}\n"

/* full resolution luma plane, BT.601 limited range */
static const char *FS_Y = DEMOSAIC_PRELUDE DEMOSAIC_BILINEAR COLORIZE
    "out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    ivec2 o = ivec2(gl_FragCoord.xy);\n"
    "    vec3 rgb = colorize(demosaic(mapSrc(o)));\n"
    "    float y = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;\n"
    "    fragColor = vec4((16.0 + 219.0 * y) / 255.0);\n"
    "}\n";

/* half resolution chroma: average the 4x4 RAW neighbourhood of each 2x2 block */
static const char *FS_UV = DEMOSAIC_PRELUDE COLORIZE
    "uniform highp vec3 u_cnt;\n"
    "out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    ivec2 uvp = ivec2(gl_FragCoord.xy);\n"
    "    ivec2 o0 = uvp * 2;\n"
    "    ivec2 r0 = mapSrc(o0);\n"
    "    ivec2 r1 = mapSrc(o0 + ivec2(1, 1));\n"
    "    ivec2 base = ivec2(min(r0.x, r1.x) & ~1, min(r0.y, r1.y) & ~1);\n"
    "    ivec2 start = base - ivec2(1, 1);\n"
    "    float sumR = 0.0;\n"
    "    float sumG = 0.0;\n"
    "    float sumB = 0.0;\n"
    "    for (int yy = 0; yy < 4; ++yy) {\n"
    "        for (int xx = 0; xx < 4; ++xx) {\n"
    "            ivec2 p = start + ivec2(xx, yy);\n"
    "            float v = rawFetch(p);\n"
    "            int c = colorAt(p);\n"
    "            if (c == 0)      sumR += v;\n"
    "            else if (c == 1) sumG += v;\n"
    "            else             sumB += v;\n"
    "        }\n"
    "    }\n"
    "    vec3 rgb = colorize(vec3(sumR * u_cnt.x, sumG * u_cnt.y, sumB * u_cnt.z));\n"
    "    float cb = -0.168736 * rgb.r - 0.331264 * rgb.g + 0.5 * rgb.b;\n"
    "    float cr = 0.5 * rgb.r - 0.418688 * rgb.g - 0.081312 * rgb.b;\n"
    "    fragColor = vec4((128.0 + 224.0 * cb) / 255.0,\n"
    "                     (128.0 + 224.0 * cr) / 255.0, 0.0, 1.0);\n"
    "}\n";

/* full resolution RGB888 path (still photos) */
static const char *FS_RGB = DEMOSAIC_PRELUDE DEMOSAIC_BILINEAR COLORIZE
    "out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    ivec2 o = ivec2(gl_FragCoord.xy);\n"
    "    fragColor = vec4(colorize(demosaic(mapSrc(o))), 1.0);\n"
    "}\n";

/*
 * statistics pass A: one fragment per 8x8 RAW block (fixed, unrolled loops).
 * Two rows (one even, one odd) of the block are sampled, giving 4 samples per
 * parity class.  The block origin is always even, so the four parity classes
 * map directly to the four Bayer colours c = (0,0),(1,0),(0,1),(1,1).
 *
 * output: mean R, mean G, mean B in 0..255 scale (black level subtracted,
 * 10-bit RAW units / 4) and a coverage flag.
 */
static const char *FS_STATS =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D u_raw;\n"
    "uniform highp ivec2 u_raw_off;\n" /* even origin of the block grid */
    "uniform highp ivec4 u_c;\n"
    "const float u_black = 64.0;\n" /* must match RAW_BLACK_LEVEL in C */
    "out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    ivec2 base = ivec2(gl_FragCoord.xy) * 8 + u_raw_off;\n"
    "    float s00 = 0.0;\n" /* (x even, y even) -> colour c.x */
    "    float s10 = 0.0;\n" /* (x odd,  y even) -> colour c.y */
    "    float s01 = 0.0;\n" /* (x even, y odd)  -> colour c.z */
    "    float s11 = 0.0;\n" /* (x odd,  y odd)  -> colour c.w */
    "    for (int yy = 0; yy < 2; ++yy) {\n" /* both row parities */
    "        for (int xx = 0; xx < 8; ++xx) {\n"
    "            float v = max(texelFetch(u_raw, base + ivec2(xx, yy), 0).r - u_black, 0.0);\n"
    "            if ((yy & 1) == 0) { if ((xx & 1) == 0) s00 += v; else s10 += v; }\n"
    "            else               { if ((xx & 1) == 0) s01 += v; else s11 += v; }\n"
    "        }\n"
    "    }\n"
    "    float m00 = s00 * 0.25;\n" /* 4 samples per parity class */
    "    float m10 = s10 * 0.25;\n"
    "    float m01 = s01 * 0.25;\n"
    "    float m11 = s11 * 0.25;\n"
    "    int nR = (u_c.x == 0 ? 1 : 0) + (u_c.y == 0 ? 1 : 0) + (u_c.z == 0 ? 1 : 0) +\n"
    "             (u_c.w == 0 ? 1 : 0);\n"
    "    int nG = (u_c.x == 1 ? 1 : 0) + (u_c.y == 1 ? 1 : 0) + (u_c.z == 1 ? 1 : 0) +\n"
    "             (u_c.w == 1 ? 1 : 0);\n"
    "    int nB = (u_c.x == 2 ? 1 : 0) + (u_c.y == 2 ? 1 : 0) + (u_c.z == 2 ? 1 : 0) +\n"
    "             (u_c.w == 2 ? 1 : 0);\n"
    "    float r = ((u_c.x == 0 ? m00 : 0.0) + (u_c.y == 0 ? m10 : 0.0) +\n"
    "               (u_c.z == 0 ? m01 : 0.0) + (u_c.w == 0 ? m11 : 0.0)) / float(nR);\n"
    "    float g = ((u_c.x == 1 ? m00 : 0.0) + (u_c.y == 1 ? m10 : 0.0) +\n"
    "               (u_c.z == 1 ? m01 : 0.0) + (u_c.w == 1 ? m11 : 0.0)) / float(nG);\n"
    "    float b = ((u_c.x == 2 ? m00 : 0.0) + (u_c.y == 2 ? m10 : 0.0) +\n"
    "               (u_c.z == 2 ? m01 : 0.0) + (u_c.w == 2 ? m11 : 0.0)) / float(nB);\n"
    "    fragColor = vec4(r * 0.25, g * 0.25, b * 0.25, 1.0);\n"
    "}\n";

/* statistics pass B: fixed 4x4 box reduction, validity-weighted */
static const char *FS_STATS_DS =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D u_src;\n"
    "out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    ivec2 base = ivec2(gl_FragCoord.xy) * 4;\n"
    "    vec3 sum = vec3(0.0);\n"
    "    float wsum = 0.0;\n"
    "    for (int y = 0; y < 4; ++y) {\n"
    "        for (int x = 0; x < 4; ++x) {\n"
    "            vec4 v = texelFetch(u_src, base + ivec2(x, y), 0);\n"
    "            sum += v.rgb * v.w;\n" /* weight by coverage fraction */
    "            wsum += v.w;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(wsum > 1e-4 ? sum / wsum : vec3(0.0), wsum / 16.0);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* shader / program plumbing                                          */
/* ------------------------------------------------------------------ */

static GLuint compile_shader(GLenum type, const char *src, const char *name)
{
    GLuint sh = glCreateShader(type);
    if (!sh)
        return 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        ELISH_ERR("gpu: %s shader compile failed: %s", name, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int program_init(gpu_program *p, const char *fs_src, const char *name)
{
    memset(p, 0, sizeof(*p));
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VS_FULLSCREEN, name);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, name);
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return -1;
    }
    GLuint id = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(id, sizeof(log), NULL, log);
        ELISH_ERR("gpu: %s program link failed: %s", name, log);
        glDeleteProgram(id);
        return -1;
    }
    p->id = id;
    p->u_packed = glGetUniformLocation(id, "u_packed");
    p->u_raw = glGetUniformLocation(id, "u_raw");
    p->u_src = glGetUniformLocation(id, "u_src");
    p->u_size = glGetUniformLocation(id, "u_size");
    p->u_raw_size = glGetUniformLocation(id, "u_raw_size");
    p->u_rot = glGetUniformLocation(id, "u_rot");
    p->u_c = glGetUniformLocation(id, "u_c");
    p->u_wb = glGetUniformLocation(id, "u_wb");
    p->u_gamma = glGetUniformLocation(id, "u_gamma");
    p->u_cnt = glGetUniformLocation(id, "u_cnt");
    p->u_off = glGetUniformLocation(id, "u_raw_off");
    p->u_grid = glGetUniformLocation(id, "u_grid");
    return 0;
}

static void program_free(gpu_program *p)
{
    if (p->id)
        glDeleteProgram(p->id);
    memset(p, 0, sizeof(*p));
}

/* ------------------------------------------------------------------ */
/* dma-buf render targets (cached CPU mapping)                        */
/* ------------------------------------------------------------------ */

static void target_reset(gpu_target *t)
{
    t->fd = -1;
    t->map = NULL;
    t->size = 0;
    t->pitch = 0;
    t->w = 0;
    t->h = 0;
    t->img = EGL_NO_IMAGE_KHR;
    t->rb = 0;
}

static void target_free(elish_gpu *g, gpu_target *t)
{
    if (t->rb) {
        glDeleteRenderbuffers(1, &t->rb);
        t->rb = 0;
    }
    if (t->img != EGL_NO_IMAGE_KHR && g->destroy_image) {
        g->destroy_image(g->dpy, t->img);
        t->img = EGL_NO_IMAGE_KHR;
    }
    if (t->map) {
        munmap(t->map, t->size);
        t->map = NULL;
    }
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    t->size = 0;
    t->pitch = 0;
}

static int dmabuf_alloc(elish_gpu *g, int *fd_out, uint8_t **map_out, size_t size)
{
    int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0) {
        ELISH_ERR("gpu: cannot open /dev/dma_heap/system: %s", strerror(errno));
        return -1;
    }
    struct dma_heap_allocation_data req;
    memset(&req, 0, sizeof(req));
    req.len = (uint64_t)size;
    req.fd_flags = O_RDWR | O_CLOEXEC;
    req.heap_flags = 0;
    int rc = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req);
    int saved = errno;
    close(heap);
    if (rc < 0) {
        ELISH_ERR("gpu: dma_heap alloc %zu B failed: %s", size, strerror(saved));
        return -1;
    }
    *fd_out = (int)req.fd;
    *map_out = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd_out, 0);
    if (*map_out == MAP_FAILED) {
        *map_out = NULL;
        ELISH_ERR("gpu: mmap dma-buf failed: %s", strerror(errno));
        close(*fd_out);
        *fd_out = -1;
        return -1;
    }
    return 0;
}

/*
 * Import a (w x h) single-plane view of a dma-buf as an EGLImage renderbuffer.
 * `pitch` must be 64-byte aligned: Mesa's dma-buf import rejects other pitches.
 */
static EGLImageKHR import_image(elish_gpu *g, int fd, uint32_t fourcc, int w, int h, uint32_t pitch,
                                size_t offset, GLuint *rb_out, const char *what)
{
    EGLint attrs[] = {
        EGL_WIDTH, w,
        EGL_HEIGHT, h,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
        EGL_NONE,
    };
    EGLImageKHR img =
        g->create_image(g->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        ELISH_ERR("gpu: %s: eglCreateImageKHR(fourcc %c%c%c%c %dx%d pitch %u off %zu) failed 0x%x",
                  what, (char)(fourcc & 0xff), (char)((fourcc >> 8) & 0xff),
                  (char)((fourcc >> 16) & 0xff), (char)((fourcc >> 24) & 0xff), w, h, pitch, offset,
                  eglGetError());
        return EGL_NO_IMAGE_KHR;
    }
    GLuint rb = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    g->image_target_rb(GL_RENDERBUFFER, img);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    GLenum err = glGetError();
    if (st != GL_FRAMEBUFFER_COMPLETE || err != GL_NO_ERROR) {
        ELISH_ERR("gpu: %s: dma-buf FBO incomplete (0x%x, gl 0x%x) %dx%d fourcc %c%c%c%c", what, st,
                  err, w, h, (char)(fourcc & 0xff), (char)((fourcc >> 8) & 0xff),
                  (char)((fourcc >> 16) & 0xff), (char)((fourcc >> 24) & 0xff));
        glDeleteRenderbuffers(1, &rb);
        g->destroy_image(g->dpy, img);
        return EGL_NO_IMAGE_KHR;
    }
    *rb_out = rb;
    return img;
}

static int target_create(elish_gpu *g, gpu_target *t, uint32_t fourcc, int w, int h, int bpp,
                         const char *what)
{
    target_reset(t);
    if (w <= 0 || h <= 0)
        return -1;
    t->pitch = (uint32_t)((((uint64_t)w * (uint64_t)bpp) + 63ull) & ~63ull);
    t->size = (size_t)t->pitch * (size_t)h;
    t->w = w;
    t->h = h;
    if (dmabuf_alloc(g, &t->fd, &t->map, t->size) < 0)
        return -1;
    t->img = import_image(g, t->fd, fourcc, w, h, t->pitch, 0, &t->rb, what);
    if (t->img == EGL_NO_IMAGE_KHR)
        return -1;
    return 0;
}

static void nv12_reset(gpu_nv12 *n)
{
    n->fd = -1;
    n->map = NULL;
    n->size = 0;
    n->pitch = 0;
    n->w = 0;
    n->h = 0;
    n->uv_off = 0;
    n->img_y = EGL_NO_IMAGE_KHR;
    n->img_uv = EGL_NO_IMAGE_KHR;
    n->rb_y = 0;
    n->rb_uv = 0;
}

static void nv12_free(elish_gpu *g, gpu_nv12 *n)
{
    if (n->rb_y) {
        glDeleteRenderbuffers(1, &n->rb_y);
        n->rb_y = 0;
    }
    if (n->rb_uv) {
        glDeleteRenderbuffers(1, &n->rb_uv);
        n->rb_uv = 0;
    }
    if (n->img_y != EGL_NO_IMAGE_KHR && g->destroy_image) {
        g->destroy_image(g->dpy, n->img_y);
        n->img_y = EGL_NO_IMAGE_KHR;
    }
    if (n->img_uv != EGL_NO_IMAGE_KHR && g->destroy_image) {
        g->destroy_image(g->dpy, n->img_uv);
        n->img_uv = EGL_NO_IMAGE_KHR;
    }
    if (n->map) {
        munmap(n->map, n->size);
        n->map = NULL;
    }
    if (n->fd >= 0) {
        close(n->fd);
        n->fd = -1;
    }
    n->size = 0;
    n->pitch = 0;
}

/*
 * One dma-buf holding both planes:
 *   [ Y  : pitch x out_h  (R8) ][ pad ][ UV : pitch x out_h/2 (GR88) ]
 * pitch is 64-byte aligned (Mesa import requirement), so the caller's NV12
 * buffer (which may use stride == out_w) is assembled with row copies.
 */
static int nv12_create(elish_gpu *g, int out_w, int out_h)
{
    gpu_nv12 *n = &g->nv12;
    nv12_free(g, n);
    n->w = out_w;
    n->h = out_h;
    n->pitch = (uint32_t)((((uint64_t)out_w) + 63ull) & ~63ull);
    n->uv_off = (size_t)n->pitch * (size_t)out_h;
    n->size = n->uv_off + (size_t)n->pitch * (size_t)(out_h / 2);
    if (dmabuf_alloc(g, &n->fd, &n->map, n->size) < 0)
        return -1;
    /* Y plane */
    n->img_y = import_image(g, n->fd, GBM_FORMAT_R8, out_w, out_h, n->pitch, 0, &n->rb_y, "nv12-Y");
    if (n->img_y == EGL_NO_IMAGE_KHR)
        return -1;
    /* UV plane (interleaved U,V half resolution) */
    n->img_uv = import_image(g, n->fd, GBM_FORMAT_GR88, out_w / 2, out_h / 2, n->pitch, n->uv_off,
                             &n->rb_uv, "nv12-UV");
    if (n->img_uv == EGL_NO_IMAGE_KHR)
        return -1;
    return 0;
}

static void cpu_access_begin(int fd)
{
    struct dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    if (fd >= 0)
        ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static void cpu_access_end(int fd)
{
    struct dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (fd >= 0)
        ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* ------------------------------------------------------------------ */
/* EGL / GLES init                                                    */
/* ------------------------------------------------------------------ */

static int gpu_egl_init(elish_gpu *g)
{
    g->drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (g->drm_fd < 0) {
        ELISH_ERR("gpu: cannot open /dev/dri/renderD128: %s", strerror(errno));
        return -1;
    }
    g->gbm = gbm_create_device(g->drm_fd);
    if (!g->gbm) {
        ELISH_ERR("gpu: gbm_create_device failed");
        return -1;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display)
        g->dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, g->gbm, NULL);
    if (g->dpy == EGL_NO_DISPLAY)
        g->dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (g->dpy == EGL_NO_DISPLAY) {
        ELISH_ERR("gpu: no EGL display (GBM and surfaceless both failed)");
        return -1;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(g->dpy, &major, &minor)) {
        ELISH_ERR("gpu: eglInitialize failed (0x%x)", eglGetError());
        return -1;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        ELISH_ERR("gpu: eglBindAPI(OpenGL ES) failed");
        return -1;
    }

    EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig cfg = EGL_NO_CONFIG_KHR;
    EGLint ncfg = 0;
    eglChooseConfig(g->dpy, cfg_attr, &cfg, 1, &ncfg);
    if (ncfg < 1)
        cfg = EGL_NO_CONFIG_KHR;

    EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        EGLint ctx3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, ctx3);
    }
    if (ctx == EGL_NO_CONTEXT) {
        ELISH_ERR("gpu: eglCreateContext(ES 3.x) failed (0x%x)", eglGetError());
        return -1;
    }
    g->ctx = ctx;

    if (!eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        /* surfaceless current not supported: fall back to a tiny pbuffer */
        EGLint pb_attr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        g->surf = eglCreatePbufferSurface(g->dpy, cfg, pb_attr);
        if (g->surf == EGL_NO_SURFACE || !eglMakeCurrent(g->dpy, g->surf, g->surf, ctx)) {
            ELISH_ERR("gpu: eglMakeCurrent failed (0x%x)", eglGetError());
            return -1;
        }
    }

    g->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    g->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    g->image_target_rb =
        (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress(
            "glEGLImageTargetRenderbufferStorageOES");
    if (!g->create_image || !g->destroy_image || !g->image_target_rb) {
        ELISH_ERR("gpu: EGL/GLES dma-buf import functions unavailable");
        return -1;
    }

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    snprintf(g->renderer, sizeof(g->renderer), "%s", renderer ? renderer : "unknown");
    return 0;
}

static int gpu_programs_init(elish_gpu *g)
{
    if (program_init(&g->p_unpack, FS_UNPACK, "unpack") < 0 ||
        program_init(&g->p_y, FS_Y, "y") < 0 || program_init(&g->p_uv, FS_UV, "uv") < 0 ||
        program_init(&g->p_rgb, FS_RGB, "rgb") < 0 ||
        program_init(&g->p_stats, FS_STATS, "stats") < 0 ||
        program_init(&g->p_stats_ds, FS_STATS_DS, "stats_ds") < 0) {
        return -1;
    }
    return 0;
}

static void gpu_gl_state_init(elish_gpu *g)
{
    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);
    glGenFramebuffers(1, &g->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

/* ------------------------------------------------------------------ */
/* resource caches                                                    */
/* ------------------------------------------------------------------ */

static void cache_raw_free(elish_gpu *g)
{
    if (g->tex_pack) {
        glDeleteTextures(1, &g->tex_pack);
        g->tex_pack = 0;
    }
    g->pack_stride = 0;
    g->pack_h = 0;
    if (g->tex_raw) {
        glDeleteTextures(1, &g->tex_raw);
        g->tex_raw = 0;
    }
    g->raw_w = 0;
    g->raw_h = 0;
}

static void cache_nv12_free(elish_gpu *g)
{
    nv12_free(g, &g->nv12);
    g->nv12_w = 0;
    g->nv12_h = 0;
}

static void cache_rgb_free(elish_gpu *g)
{
    target_free(g, &g->t_rgb);
    g->rgb_w = 0;
    g->rgb_h = 0;
}

static void cache_all_free(elish_gpu *g)
{
    cache_raw_free(g);
    cache_nv12_free(g);
    cache_rgb_free(g);
}

static int ensure_raw(elish_gpu *g, int w, int h)
{
    if (g->tex_raw && g->raw_w == w && g->raw_h == h)
        return 0;
    cache_raw_free(g);
    glGenTextures(1, &g->tex_raw);
    if (!g->tex_raw)
        return -1;
    glBindTexture(GL_TEXTURE_2D, g->tex_raw);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16F, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) {
        ELISH_ERR("gpu: R16F raw texture %dx%d allocation failed", w, h);
        cache_raw_free(g);
        return -1;
    }
    g->raw_w = w;
    g->raw_h = h;
    return 0;
}

static int ensure_nv12(elish_gpu *g, int out_w, int out_h)
{
    if (g->nv12.rb_y && g->nv12_w == out_w && g->nv12_h == out_h)
        return 0;
    cache_nv12_free(g);
    if (nv12_create(g, out_w, out_h) < 0) {
        cache_nv12_free(g);
        return -1;
    }
    g->nv12_w = out_w;
    g->nv12_h = out_h;
    return 0;
}

static int ensure_rgb(elish_gpu *g, int out_w, int out_h)
{
    if (g->t_rgb.rb && g->rgb_w == out_w && g->rgb_h == out_h)
        return 0;
    cache_rgb_free(g);
    if (target_create(g, &g->t_rgb, GBM_FORMAT_ABGR8888, out_w, out_h, 4, "rgb") < 0) {
        cache_rgb_free(g);
        return -1;
    }
    g->rgb_w = out_w;
    g->rgb_h = out_h;
    return 0;
}

/* ------------------------------------------------------------------ */
/* stages                                                             */
/* ------------------------------------------------------------------ */

static int upload_raw(elish_gpu *g, const elish_raw_image *raw)
{
    if (!g->tex_pack || g->pack_stride != raw->stride || g->pack_h != raw->height) {
        if (g->tex_pack) {
            glDeleteTextures(1, &g->tex_pack);
            g->tex_pack = 0;
        }
        glGenTextures(1, &g->tex_pack);
        glBindTexture(GL_TEXTURE_2D, g->tex_pack);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, raw->stride, raw->height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (glGetError() != GL_NO_ERROR) {
            ELISH_ERR("gpu: packed R8UI texture %dx%d allocation failed", raw->stride,
                      raw->height);
            return -1;
        }
        g->pack_stride = raw->stride;
        g->pack_h = raw->height;
    }
    glBindTexture(GL_TEXTURE_2D, g->tex_pack);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, raw->stride, raw->height, GL_RED_INTEGER,
                    GL_UNSIGNED_BYTE, raw->data);
    if (glGetError() != GL_NO_ERROR) {
        ELISH_ERR("gpu: RAW10 upload failed");
        return -1;
    }
    return 0;
}

/* pass 1: packed bytes -> R16F RAW values */
static void render_unpack(elish_gpu *g, const elish_raw_image *raw)
{
    gpu_program *p = &g->p_unpack;
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->tex_raw, 0);
    glViewport(0, 0, raw->width, raw->height);
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->tex_pack);
    glUniform1i(p->u_packed, 0);
    glUniform2i(p->u_size, raw->width, raw->height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void bayer_colors(elish_bayer bayer, int c[4])
{
    switch (bayer) {
    case ELISH_BAYER_RGGB: /* R G / G B */
        c[0] = 0;
        c[1] = 1;
        c[2] = 1;
        c[3] = 2;
        break;
    case ELISH_BAYER_GRBG: /* G R / B G */
        c[0] = 1;
        c[1] = 0;
        c[2] = 2;
        c[3] = 1;
        break;
    case ELISH_BAYER_GBRG: /* G B / R G */
        c[0] = 1;
        c[1] = 2;
        c[2] = 0;
        c[3] = 1;
        break;
    case ELISH_BAYER_BGGR: /* B G / G R */
    default:
        c[0] = 2;
        c[1] = 1;
        c[2] = 1;
        c[3] = 0;
        break;
    }
}

static void bind_nv12_y(elish_gpu *g)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              g->nv12.rb_y);
    glViewport(0, 0, g->nv12.w, g->nv12.h);
}

static void bind_nv12_uv(elish_gpu *g)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              g->nv12.rb_uv);
    glViewport(0, 0, g->nv12.w / 2, g->nv12.h / 2);
}

static void set_demosaic_uniforms(elish_gpu *g, gpu_program *p, const elish_raw_image *raw,
                                  int rotation, elish_wb wb, float gamma, elish_bayer bayer)
{
    int c[4];
    bayer_colors(bayer, c);
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->tex_raw);
    glUniform1i(p->u_raw, 0);
    glUniform2i(p->u_raw_size, raw->width, raw->height);
    glUniform1i(p->u_rot, rotation);
    glUniform4i(p->u_c, c[0], c[1], c[2], c[3]);
    glUniform3f(p->u_wb, wb.r, wb.g, wb.b);
    glUniform1f(p->u_gamma, gamma);
    if (p->u_cnt >= 0) {
        int npos[3] = {0, 0, 0};
        for (int i = 0; i < 4; ++i)
            npos[c[i] & 3]++;
        float inv[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 3; ++i)
            inv[i] = npos[i] ? 1.0f / (4.0f * (float)npos[i]) : 0.0f;
        glUniform3f(p->u_cnt, inv[0], inv[1], inv[2]);
    }
}

static void draw_fullscreen(void)
{
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void render_y(elish_gpu *g, const elish_raw_image *raw, int rotation, elish_wb wb,
                     float gamma, elish_bayer bayer)
{
    bind_nv12_y(g);
    set_demosaic_uniforms(g, &g->p_y, raw, rotation, wb, gamma, bayer);
    draw_fullscreen();
}

static void render_uv(elish_gpu *g, const elish_raw_image *raw, int rotation, elish_wb wb,
                      float gamma, elish_bayer bayer)
{
    bind_nv12_uv(g);
    set_demosaic_uniforms(g, &g->p_uv, raw, rotation, wb, gamma, bayer);
    draw_fullscreen();
}

static void render_rgb(elish_gpu *g, const elish_raw_image *raw, int rotation, elish_wb wb,
                       float gamma, elish_bayer bayer)
{
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              g->t_rgb.rb);
    glViewport(0, 0, g->t_rgb.w, g->t_rgb.h);
    set_demosaic_uniforms(g, &g->p_rgb, raw, rotation, wb, gamma, bayer);
    draw_fullscreen();
}

/* ------------------------------------------------------------------ */
/* CPU-side copies                                                    */
/* ------------------------------------------------------------------ */

static void copy_plane_rows(uint8_t *dst, int dst_stride, const uint8_t *src, uint32_t src_pitch,
                            int row_bytes, int rows)
{
    for (int y = 0; y < rows; ++y)
        memcpy(dst + (size_t)y * (size_t)dst_stride, src + (size_t)y * (size_t)src_pitch,
               (size_t)row_bytes);
}

static void rgba_to_rgb(uint8_t *dst, int dst_stride, const uint8_t *src, uint32_t src_pitch, int w,
                        int h)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = src + (size_t)y * (size_t)src_pitch;
        uint8_t *d = dst + (size_t)y * (size_t)dst_stride;
        int x = 0;
#if defined(__aarch64__)
        for (; x + 16 <= w; x += 16) {
            uint8x16x4_t v = vld4q_u8(s + (size_t)x * 4);
            uint8x16x3_t o;
            o.val[0] = v.val[0];
            o.val[1] = v.val[1];
            o.val[2] = v.val[2];
            vst3q_u8(d + (size_t)x * 3, o);
        }
#endif
        for (; x < w; ++x) {
            d[(size_t)x * 3 + 0] = s[(size_t)x * 4 + 0];
            d[(size_t)x * 3 + 1] = s[(size_t)x * 4 + 1];
            d[(size_t)x * 3 + 2] = s[(size_t)x * 4 + 2];
        }
    }
}

/* ------------------------------------------------------------------ */
/* argument validation                                                */
/* ------------------------------------------------------------------ */

static int valid_raw(const elish_raw_image *raw)
{
    if (!raw || !raw->data)
        return 0;
    if (raw->width <= 0 || raw->height <= 0 || raw->width > MAX_DIM || raw->height > MAX_DIM)
        return 0;
    int min_stride = (raw->width * 10 + 7) / 8; /* ceil(W*5/4) */
    if (raw->stride < min_stride)
        return 0;
    return 1;
}

static int valid_rotation(elish_bayer bayer, int rotation, int raw_w, int raw_h, int out_w,
                          int out_h, int need_even)
{
    if (bayer < ELISH_BAYER_RGGB || bayer > ELISH_BAYER_BGGR)
        return 0;
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
        return 0;
    if (rotation == 90 || rotation == 270) {
        if (out_w != raw_h || out_h != raw_w)
            return 0;
    } else {
        if (out_w != raw_w || out_h != raw_h)
            return 0;
    }
    if (out_w <= 0 || out_h <= 0 || out_w > MAX_DIM || out_h > MAX_DIM)
        return 0;
    if (need_even && ((out_w & 1) || (out_h & 1)))
        return 0;
    return 1;
}

static float sanitize_gamma(float gamma)
{
    if (!(gamma > 0.0f) || !isfinite(gamma))
        return 1.0f; /* no-op gamma for invalid input */
    return gamma;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

elish_gpu *elish_gpu_create(void)
{
    elish_gpu *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->drm_fd = -1;
    target_reset(&g->t_rgb);
    nv12_reset(&g->nv12);
    const char *prof = getenv("ELISH_GPU_PROFILE");
    g->profile = (prof && *prof) ? 1 : 0;

    if (gpu_egl_init(g) < 0 || gpu_programs_init(g) < 0)
        goto fail;
    gpu_gl_state_init(g);

    /* statistics reduction chain: 1024x1024 -> 256x256 -> 64x64 RGBA16F */
    GLenum st_a = 0, st_b = 0, st_c = 0;
    glGenTextures(1, &g->stats_tex);
    glBindTexture(GL_TEXTURE_2D, g->stats_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, STATS_GRID_A, STATS_GRID_A);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g->stats_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->stats_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->stats_tex, 0);
    st_a = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glGenTextures(1, &g->stats_tex2);
    glBindTexture(GL_TEXTURE_2D, g->stats_tex2);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, STATS_GRID_B, STATS_GRID_B);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g->stats_fbo2);
    glBindFramebuffer(GL_FRAMEBUFFER, g->stats_fbo2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->stats_tex2, 0);
    st_b = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glGenRenderbuffers(1, &g->stats_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, g->stats_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA16F, STATS_OUT, STATS_OUT);
    glGenFramebuffers(1, &g->stats_fbo_out);
    glBindFramebuffer(GL_FRAMEBUFFER, g->stats_fbo_out);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g->stats_rb);
    st_c = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st_a != GL_FRAMEBUFFER_COMPLETE || st_b != GL_FRAMEBUFFER_COMPLETE ||
        st_c != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR) {
        ELISH_ERR("gpu: statistics framebuffers allocation failed (0x%x/0x%x/0x%x)", st_a, st_b,
                  st_c);
        goto fail;
    }
    /*
     * Clear the reduction targets once: fragments outside the rendered RAW crop
     * keep coverage 0 and are ignored by the following reduction steps.
     */
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, g->stats_fbo);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, g->stats_fbo2);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (g->profile)
        ELISH_LOG("gpu: renderer=%s (profiling enabled)", g->renderer);
    return g;

fail:
    elish_gpu_destroy(g);
    return NULL;
}

void elish_gpu_destroy(elish_gpu *g)
{
    if (!g)
        return;
    if (g->ctx != EGL_NO_CONTEXT && g->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        cache_all_free(g);
        if (g->stats_rb) {
            glDeleteRenderbuffers(1, &g->stats_rb);
            g->stats_rb = 0;
        }
        if (g->stats_fbo_out) {
            glDeleteFramebuffers(1, &g->stats_fbo_out);
            g->stats_fbo_out = 0;
        }
        if (g->stats_fbo2) {
            glDeleteFramebuffers(1, &g->stats_fbo2);
            g->stats_fbo2 = 0;
        }
        if (g->stats_tex2) {
            glDeleteTextures(1, &g->stats_tex2);
            g->stats_tex2 = 0;
        }
        if (g->stats_fbo) {
            glDeleteFramebuffers(1, &g->stats_fbo);
            g->stats_fbo = 0;
        }
        if (g->stats_tex) {
            glDeleteTextures(1, &g->stats_tex);
            g->stats_tex = 0;
        }
        if (g->fbo) {
            glDeleteFramebuffers(1, &g->fbo);
            g->fbo = 0;
        }
        if (g->vao) {
            glDeleteVertexArrays(1, &g->vao);
            g->vao = 0;
        }
        program_free(&g->p_unpack);
        program_free(&g->p_y);
        program_free(&g->p_uv);
        program_free(&g->p_rgb);
        program_free(&g->p_stats);
        program_free(&g->p_stats_ds);
        if (g->surf != EGL_NO_SURFACE)
            eglDestroySurface(g->dpy, g->surf);
        eglDestroyContext(g->dpy, g->ctx);
        eglTerminate(g->dpy);
    }
    if (g->gbm)
        gbm_device_destroy(g->gbm);
    if (g->drm_fd >= 0)
        close(g->drm_fd);
    free(g);
}

const char *elish_gpu_renderer(elish_gpu *g)
{
    return g ? g->renderer : NULL;
}

int elish_gpu_debayer_nv12(elish_gpu *g, const elish_raw_image *raw, elish_bayer bayer,
                           int out_w, int out_h, int rotation, elish_wb wb, float gamma,
                           uint8_t *nv12, int nv12_stride)
{
    if (!g || !valid_raw(raw) || !nv12)
        return -EINVAL;
    if (nv12_stride < out_w || (nv12_stride & 1))
        return -EINVAL;
    if (!valid_rotation(bayer, rotation, raw->width, raw->height, out_w, out_h, 1))
        return -EINVAL;
    gamma = sanitize_gamma(gamma);

    uint64_t t0 = g->profile ? tick_ns() : 0;

    if (ensure_raw(g, raw->width, raw->height) < 0)
        return -ENOMEM;
    if (ensure_nv12(g, out_w, out_h) < 0)
        return -ENOMEM;
    if (upload_raw(g, raw) < 0)
        return -EIO;
    PROF_MARK(g, t0, "upload");

    render_unpack(g, raw);
    PROF_MARK(g, t0, "unpack");

    render_y(g, raw, rotation, wb, gamma, bayer);
    PROF_MARK(g, t0, "y");

    render_uv(g, raw, rotation, wb, gamma, bayer);
    glFinish();
    PROF_MARK(g, t0, "uv");

    /* GPU -> CPU: the dma-buf mapping is cached, so this is a plain memcpy. */
    cpu_access_begin(g->nv12.fd);
    PROF_MARK(g, t0, "sync");
    copy_plane_rows(nv12, nv12_stride, g->nv12.map, g->nv12.pitch, out_w, out_h);
    copy_plane_rows(nv12 + (size_t)nv12_stride * (size_t)out_h, nv12_stride,
                    g->nv12.map + g->nv12.uv_off, g->nv12.pitch, out_w, out_h / 2);
    PROF_MARK(g, t0, "memcpy");
    cpu_access_end(g->nv12.fd);
    PROF_MARK(g, t0, "syncend");
    return 0;
}

int elish_gpu_debayer_rgb(elish_gpu *g, const elish_raw_image *raw, elish_bayer bayer,
                          int out_w, int out_h, int rotation, elish_wb wb, float gamma,
                          uint8_t *rgb, int rgb_stride)
{
    if (!g || !valid_raw(raw) || !rgb)
        return -EINVAL;
    if (rgb_stride < out_w * 3 || rgb_stride <= 0)
        return -EINVAL;
    if (!valid_rotation(bayer, rotation, raw->width, raw->height, out_w, out_h, 0))
        return -EINVAL;
    gamma = sanitize_gamma(gamma);

    uint64_t t0 = g->profile ? tick_ns() : 0;

    if (ensure_raw(g, raw->width, raw->height) < 0)
        return -ENOMEM;
    if (ensure_rgb(g, out_w, out_h) < 0)
        return -ENOMEM;
    if (upload_raw(g, raw) < 0)
        return -EIO;
    PROF_MARK(g, t0, "upload");

    render_unpack(g, raw);
    PROF_MARK(g, t0, "unpack");

    render_rgb(g, raw, rotation, wb, gamma, bayer);
    glFinish();
    PROF_MARK(g, t0, "render");

    cpu_access_begin(g->t_rgb.fd);
    rgba_to_rgb(rgb, rgb_stride, g->t_rgb.map, g->t_rgb.pitch, out_w, out_h);
    cpu_access_end(g->t_rgb.fd);
    PROF_MARK(g, t0, "copy");
    return 0;
}

int elish_gpu_stats(elish_gpu *g, const elish_raw_image *raw, elish_bayer bayer, elish_wb wb,
                    elish_stats *out)
{
    if (!g || !valid_raw(raw) || !out)
        return -EINVAL;
    if (bayer < ELISH_BAYER_RGGB || bayer > ELISH_BAYER_BGGR)
        return -EINVAL;

    uint64_t t0 = g->profile ? tick_ns() : 0;

    if (ensure_raw(g, raw->width, raw->height) < 0)
        return -ENOMEM;
    if (upload_raw(g, raw) < 0)
        return -EIO;
    PROF_MARK(g, t0, "upload");

    render_unpack(g, raw);
    PROF_MARK(g, t0, "unpack");

    int c[4];
    bayer_colors(bayer, c);

    /*
     * pass A: one 4x4 RAW block per fragment, centred crop of at most
     * STATS_GRID_A*4 = 4096 pixels per axis.  The block origin is even so the
     * shader's four parity classes map directly onto the Bayer colours.
     */
    int gw = raw->width / 8;
    if (gw > STATS_GRID_A)
        gw = STATS_GRID_A;
    if (gw < 1)
        gw = 1;
    int gh = raw->height / 8;
    if (gh > STATS_GRID_A)
        gh = STATS_GRID_A;
    if (gh < 1)
        gh = 1;
    int off_x = (raw->width - gw * 8) / 2;
    int off_y = (raw->height - gh * 8) / 2;
    off_x &= ~1;
    off_y &= ~1;
    if (off_x < 0)
        off_x = 0;
    if (off_y < 0)
        off_y = 0;

    glBindFramebuffer(GL_FRAMEBUFFER, g->stats_fbo);
    glViewport(0, 0, gw, gh);
    glUseProgram(g->p_stats.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->tex_raw);
    glUniform1i(g->p_stats.u_raw, 0);
    glUniform2i(g->p_stats.u_off, off_x, off_y);
    glUniform4i(g->p_stats.u_c, c[0], c[1], c[2], c[3]);
    draw_fullscreen();
    PROF_MARK(g, t0, "statsA");

    /* pass B1: 1024x1024 -> 256x256 fixed 4x4 box reduction */
    glBindFramebuffer(GL_FRAMEBUFFER, g->stats_fbo2);
    glViewport(0, 0, STATS_GRID_B, STATS_GRID_B);
    glUseProgram(g->p_stats_ds.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->stats_tex);
    glUniform1i(g->p_stats_ds.u_src, 0);
    draw_fullscreen();
    PROF_MARK(g, t0, "statsB1");

    /* pass B2: 256x256 -> 64x64 */
    glBindFramebuffer(GL_FRAMEBUFFER, g->stats_fbo_out);
    glViewport(0, 0, STATS_OUT, STATS_OUT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->stats_tex2);
    draw_fullscreen();
    glFinish();
    PROF_MARK(g, t0, "statsB2");

    float buf[STATS_OUT * STATS_OUT * 4];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, STATS_OUT, STATS_OUT, GL_RGBA, GL_FLOAT, buf);
    if (glGetError() != GL_NO_ERROR) {
        ELISH_ERR("gpu: statistics readback failed");
        return -EIO;
    }
    PROF_MARK(g, t0, "statsRB");

    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    double wsum = 0.0;
    for (int i = 0; i < STATS_OUT * STATS_OUT; ++i) {
        double w = (double)buf[i * 4 + 3];
        if (w <= 0.0)
            continue;
        sum_r += (double)buf[i * 4 + 0] * w;
        sum_g += (double)buf[i * 4 + 1] * w;
        sum_b += (double)buf[i * 4 + 2] * w;
        wsum += w;
    }
    if (wsum <= 0.0)
        return -EIO;

    /*
     * The shader already returned black-level-subtracted linear means in 0..255
     * scale (10-bit RAW / 4).  White balance is applied to the means here so AE
     * and AWB both see the same (linearly scaled) sensor response.
     */
    double r = sum_r / wsum * (double)wb.r;
    double gg = sum_g / wsum * (double)wb.g;
    double b = sum_b / wsum * (double)wb.b;
    double y = 0.299 * r + 0.587 * gg + 0.114 * b;
    out->mean_r = (float)r;
    out->mean_g = (float)gg;
    out->mean_b = (float)b;
    out->mean_y = (float)y;
    return 0;
}
