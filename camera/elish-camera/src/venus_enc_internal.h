/*
 * elish-camera: private introspection for the Venus encoder.
 *
 * NOT part of the public API (include/elish_camera.h).  tools/enc_test.c
 * includes this header directly to report encoder-side counters (number of
 * encoded frames, keyframe count from V4L2 buffer flags, real padded frame
 * geometry) that the streaming API deliberately does not expose.
 *
 * File owner: venus-encoder (same as src/venus_enc.c).
 */
#ifndef ELISH_VENUS_ENC_INTERNAL_H
#define ELISH_VENUS_ENC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <elish_camera.h>

#ifdef __cplusplus
extern "C" {
#endif

struct elish_enc_stats {
    uint64_t frames_pushed;  /* frames handed to elish_enc_push_nv12() */
    uint64_t frames_encoded; /* encoded frames written to the ES file */
    uint64_t keyframes;      /* encoded pictures flagged KEYFRAME (IDR/I) */
    uint64_t skipped;        /* capture buffers returned empty by the firmware */
    uint64_t bytes;          /* bytes written to the elementary stream */
    int width;               /* requested picture size (bitstream size) */
    int height;
    int in_width;  /* real V4L2 OUTPUT frame size (128x32 aligned) */
    int in_height;
    int stride;    /* real luma stride of the V4L2 OUTPUT buffers */
    size_t in_frame_size; /* padded NV12 frame size fed to the firmware */
    int fps;
    int bitrate;
    int gop;
};

/* Returns 0 and fills *out (may be called before start/at any time). */
int elish_enc_get_stats(const elish_encoder *enc, struct elish_enc_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* ELISH_VENUS_ENC_INTERNAL_H */
