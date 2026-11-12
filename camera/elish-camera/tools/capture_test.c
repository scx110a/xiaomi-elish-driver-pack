/* capture_test.c - validate raw capture on its own (Lead-owned tool). */
#include "elish_camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_size(const char *s, int *w, int *h)
{
    return sscanf(s, "%dx%d", w, h) == 2 ? 0 : -1;
}

/* quick 2x2 debayer, good enough to check geometry/orientation */
static void dump_ppm(const elish_raw_image *img, elish_bayer bayer, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", img->width / 2, img->height / 2);
    for (int y = 0; y < img->height; y += 2) {
        const uint8_t *row0 = img->data + (size_t)y * img->stride;
        const uint8_t *row1 = img->data + (size_t)(y + 1) * img->stride;
        for (int x = 0; x < img->width; x += 2) {
            unsigned px[2][2];
            for (int dy = 0; dy < 2; dy++) {
                const uint8_t *row = dy ? row1 : row0;
                for (int dx = 0; dx < 2; dx++) {
                    int xx = x + dx;
                    size_t byte = ((size_t)xx / 4) * 5;
                    unsigned b0 = row[byte + 0], b1 = row[byte + 1], b2 = row[byte + 2],
                             b3 = row[byte + 3], b4 = row[byte + 4];
                    unsigned v;
                    switch (xx % 4) {
                    case 0: v = (b0 << 2) | ((b4 >> 0) & 3); break;
                    case 1: v = (b1 << 2) | ((b4 >> 2) & 3); break;
                    case 2: v = (b2 << 2) | ((b4 >> 4) & 3); break;
                    default: v = (b3 << 2) | ((b4 >> 6) & 3); break;
                    }
                    px[dy][dx] = v >> 2;
                }
            }
            /* GRBG: (0,0)=G (0,1)=R (1,0)=B (1,1)=G */
            unsigned r, g, b;
            if (bayer == ELISH_BAYER_GRBG) {
                r = px[0][1];
                g = (px[0][0] + px[1][1]) / 2;
                b = px[1][0];
            } else { /* BGGR */
                r = px[1][1];
                g = (px[0][1] + px[1][0]) / 2;
                b = px[0][0];
            }
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            fputc(r, f);
            fputc(g, f);
            fputc(b, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    elish_cam_id id = ELISH_CAM_REAR;
    int w = 0, h = 0, frames = 30;
    const char *out = "capture";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--camera") && i + 1 < argc)
            id = !strcmp(argv[++i], "front") ? ELISH_CAM_FRONT : ELISH_CAM_REAR;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            parse_size(argv[++i], &w, &h);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
    }

    elish_camera *cam = elish_cam_open(id, w, h, 30);
    if (!cam) {
        fprintf(stderr, "open camera failed\n");
        return 1;
    }
    const elish_cam_info *info = elish_cam_info_get(cam);
    printf("camera=%s mode=%dx%d fps=%d bayer=%s video=%s sensor=%s\n",
           elish_cam_name(id), info->width, info->height, info->fps,
           elish_bayer_name(info->bayer), info->video_dev, info->sensor_subdev);

    if (elish_cam_start(cam) < 0)
        return 1;

    char raw_path[256], ppm_path[256];
    snprintf(raw_path, sizeof(raw_path), "%s.raw", out);
    snprintf(ppm_path, sizeof(ppm_path), "%s.ppm", out);

    uint64_t t0 = elish_now_ns(), t_first = 0;
    int got = 0;
    for (int i = 0; i < frames; i++) {
        elish_raw_image img;
        uint64_t ts = 0;
        if (elish_cam_get_frame(cam, &img, &ts) < 0)
            break;
        if (i == 0)
            t_first = elish_now_ns();
        if (i == 0) {
            FILE *f = fopen(raw_path, "wb");
            if (f) {
                size_t sz = (size_t)img.stride * img.height;
                fwrite(img.data, 1, sz, f);
                fclose(f);
            }
        }
        elish_cam_put_frame(cam);
        got++;
    }
    uint64_t t1 = elish_now_ns();
    if (got > 1) {
        double dt = (double)(t1 - t_first) / 1e9;
        printf("captured=%d first_wait=%.2fms fps=%.2f\n", got,
               (double)(t_first - t0) / 1e6, (got - 1) / dt);
    }
    if (frames > 0) {
        /* last frame is gone; re-capture one for the preview */
        elish_raw_image img;
        if (elish_cam_get_frame(cam, &img, NULL) == 0) {
            dump_ppm(&img, info->bayer, ppm_path);
            printf("wrote %s %s\n", raw_path, ppm_path);
            elish_cam_put_frame(cam);
        }
    }
    elish_cam_stop(cam);
    elish_cam_close(cam);
    return 0;
}
