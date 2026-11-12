/*
 * elish-camera: full-resolution front/rear camera pipeline for
 * Xiaomi Pad 5 Pro (SM8250 / elish), mainline Linux.
 *
 * Pipeline: CAMSS raw (packed MIPI RAW10) -> GPU debayer (fd650 / EGL-GLES)
 *           -> NV12 -> { Venus hardware encoder, v4l2loopback virtual camera }
 *
 * All modules are plain C11. Test images live in testdata/ (see docs/architecture.md).
 */
#ifndef ELISH_CAMERA_H
#define ELISH_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Common types                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    ELISH_CAM_FRONT = 0, /* OV8856, CSIPHY4 / CCI1, 3280x2464 */
    ELISH_CAM_REAR  = 1, /* OV13B10, CSIPHY1 / CCI0, 4208x3120 */
} elish_cam_id;

/* 2x2 Bayer pattern of the *actual* sensor readout, row-major from (0,0). */
typedef enum {
    ELISH_BAYER_RGGB = 0, /* R G / G B */
    ELISH_BAYER_GRBG = 1, /* G R / B G */
    ELISH_BAYER_GBRG = 2, /* G B / R G */
    ELISH_BAYER_BGGR = 3, /* B G / G R */
} elish_bayer;

/*
 * Raw Bayer image as delivered by CAMSS VFE RDI.
 * Packed MIPI RAW10: 4 pixels in 5 bytes.  For every group of 4 pixels:
 *   b[0] = p0[9:2], b[1] = p1[9:2], b[2] = p2[9:2], b[3] = p3[9:2],
 *   b[4] = p0[1:0]<<6 | p1[1:0]<<4 | p2[1:0]<<2 | p3[1:0]
 * Sample values are 0..1023 (10 bit), black level is NOT subtracted.
 */
typedef struct {
    const uint8_t *data; /* stride * height bytes */
    int width;           /* pixels */
    int height;          /* rows */
    int stride;          /* bytes per row, >= width*5/4, 16/64-byte aligned */
} elish_raw_image;

/* White balance gains, green is the reference (usually 1.0). */
typedef struct {
    float r;
    float g;
    float b;
} elish_wb;

typedef struct {
    float mean_r;
    float mean_g;
    float mean_b;
    float mean_y;
} elish_stats;

/* ------------------------------------------------------------------ */
/* capture.c - CAMSS raw capture + sensor controls (Lead-owned)       */
/* ------------------------------------------------------------------ */

typedef struct elish_camera elish_camera;

typedef struct {
    elish_cam_id id;
    int width;          /* active sensor mode width in pixels */
    int height;
    int fps;            /* nominal sensor fps for this mode */
    elish_bayer bayer;  /* actual Bayer pattern of the raw data */
    char video_dev[32]; /* e.g. /dev/video0 */
    char sensor_subdev[64];
    /* sensor control ranges (filled from VIDIOC_QUERYCTRL) */
    int exposure_min, exposure_max;
    int analogue_gain_min, analogue_gain_max;
    int digital_gain_min, digital_gain_max;
} elish_cam_info;

/*
 * Open + configure the media graph and the sensor mode.  Serialises access to
 * /dev/media0 with an flock lock (see docs/architecture.md).  Only one camera
 * can be open at a time because both sensors share CSID0/VFE0 on this DTB.
 */
elish_camera *elish_cam_open(elish_cam_id id, int width, int height, int fps);
const elish_cam_info *elish_cam_info_get(elish_camera *cam);

int elish_cam_start(elish_camera *cam);
/* Blocks until a frame is ready.  *img points into the mmap'ed V4L2 buffer and
 * stays valid until elish_cam_put_frame(). */
int elish_cam_get_frame(elish_camera *cam, elish_raw_image *img, uint64_t *ts_ns);
int elish_cam_put_frame(elish_camera *cam);

/* exposure is in lines, analogue_gain/digital_gain are sensor-specific units. */
int elish_cam_set_exposure(elish_camera *cam, int exposure, int analogue_gain, int digital_gain);
/* CN3927V VCM position 0..1023 (only meaningful for the rear camera). */
int elish_cam_set_focus(elish_camera *cam, int position);

void elish_cam_stop(elish_camera *cam);
void elish_cam_close(elish_camera *cam);

/* ------------------------------------------------------------------ */
/* gpu_debayer.c - EGL/GLES GPU demosaic + colour pipeline            */
/* ------------------------------------------------------------------ */

typedef struct elish_gpu elish_gpu;

elish_gpu *elish_gpu_create(void);
void elish_gpu_destroy(elish_gpu *gpu);
/* Returns the GL_RENDERER string (e.g. "FD650"), or NULL. */
const char *elish_gpu_renderer(elish_gpu *gpu);

/*
 * Demosaic raw -> NV12 (BT.601, limited range by default) with white balance
 * and gamma applied.  rotation: 0, 90, 180 or 270 degrees clockwise.
 * out_w/out_h describe the *final* orientation (for 90/270 they swap w/h).
 * nv12_stride is the Y row stride in bytes; the UV plane follows after
 * nv12_stride * out_h bytes and uses the same stride.
 */
int elish_gpu_debayer_nv12(elish_gpu *gpu, const elish_raw_image *raw, elish_bayer bayer,
                           int out_w, int out_h, int rotation, elish_wb wb, float gamma,
                           uint8_t *nv12, int nv12_stride);

/* Demosaic raw -> packed RGB888 (for still photos). */
int elish_gpu_debayer_rgb(elish_gpu *gpu, const elish_raw_image *raw, elish_bayer bayer,
                          int out_w, int out_h, int rotation, elish_wb wb, float gamma,
                          uint8_t *rgb, int rgb_stride);

/* Scene statistics on a small GPU-downscaled RGB image (before gamma). */
int elish_gpu_stats(elish_gpu *gpu, const elish_raw_image *raw, elish_bayer bayer,
                    elish_wb wb, elish_stats *out);

/* ------------------------------------------------------------------ */
/* venus_enc.c - Qualcomm Venus hardware H.264/HEVC encoder           */
/* ------------------------------------------------------------------ */

typedef enum {
    ELISH_CODEC_H264 = 0,
    ELISH_CODEC_HEVC = 1,
} elish_codec;

typedef struct elish_encoder elish_encoder;

/* es_path: raw elementary stream (.h264/.h265) written by the encoder. */
elish_encoder *elish_enc_create(elish_codec codec, int width, int height, int fps,
                                int bitrate_bps, const char *es_path);
int elish_enc_start(elish_encoder *enc);
/* Pushes one NV12 frame (synchronous copy into a free V4L2 output buffer). */
int elish_enc_push_nv12(elish_encoder *enc, const uint8_t *nv12, int stride,
                        uint64_t pts_ns, bool force_keyframe);
/* Flush + stop streaming; may be called before destroy. */
int elish_enc_stop(elish_encoder *enc);
void elish_enc_destroy(elish_encoder *enc);

/* Mux the elementary stream into MP4 using the host ffmpeg (stream copy). */
int elish_mux_mp4(const char *es_path, const char *mp4_path, elish_codec codec);

/* ------------------------------------------------------------------ */
/* loopback.c - standard V4L2 virtual camera (v4l2loopback output)    */
/* ------------------------------------------------------------------ */

typedef struct elish_loopback elish_loopback;

/* dev e.g. /dev/video2.  Producer side is V4L2 OUTPUT, consumer side is
 * V4L2 CAPTURE with NV12 at width x height. */
elish_loopback *elish_lb_open(const char *dev, int width, int height, int fps);
int elish_lb_push_nv12(elish_loopback *lb, const uint8_t *nv12, int stride);
void elish_lb_close(elish_loopback *lb);

/* ------------------------------------------------------------------ */
/* pwcam.c - PipeWire camera source node (media.role=Camera)          */
/* ------------------------------------------------------------------ */

typedef struct elish_pwcam elish_pwcam;

/*
 * Publishes an NV12 video source node straight into PipeWire with
 * media.class=Video/Source and media.role=Camera.  Unlike the v4l2loopback
 * nodes, this one advertises its formats, which portal camera apps
 * (GStreamer/aperture based, e.g. GNOME Snapshot) require.
 */
elish_pwcam *elish_pwcam_create(const char *name, const char *description,
                                const char *location, int width, int height, int fps);
/* True while a consumer is streaming from this node. */
bool elish_pwcam_active(elish_pwcam *cam);
/* Copies the latest NV12 frame into the node's buffer (no-op when inactive). */
int elish_pwcam_push(elish_pwcam *cam, const uint8_t *nv12, int stride);
void elish_pwcam_destroy(elish_pwcam *cam);

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

const char *elish_cam_name(elish_cam_id id);
const char *elish_bayer_name(elish_bayer bayer);
uint64_t elish_now_ns(void);

#define ELISH_ERR(fmt, ...) fprintf(stderr, "elish-camera: " fmt "\n", ##__VA_ARGS__)
#define ELISH_LOG(fmt, ...) fprintf(stderr, "elish-camera: " fmt "\n", ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* ELISH_CAMERA_H */
