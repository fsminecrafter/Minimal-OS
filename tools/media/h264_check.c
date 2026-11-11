// Decodes an Annex B H.264 stream (access units separated by access unit
// delimiters) with mosmedia_h264.h and writes raw yuv420p frames, for
// tools/media/h264_check.sh to compare with ffmpeg's decoder.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mosmedia_h264.h"

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: h264_check in.h264 out.yuv\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc((size_t)n + 4);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    fclose(f);
    FILE* o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); return 2; }
    h264_t h;
    if (!h264_init(&h, malloc, (size_t)n)) return 2;
    long start = 0;
    int frames = 0;
    for (long i = 0; i <= n; i++) {
        int aud = i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 31) == 9;
        if (!(aud && i > start) && i != n) continue;
        if (h264_decode(&h, d + start, (size_t)(i - start))) {
            const h264_sps_t* s = h.active;
            uint32_t W = h264_width(&h), H = h264_height(&h);
            for (uint32_t y = 0; y < H; y++)
                fwrite(h.out->plane[0] + (size_t)(y + s->crop_t) * h.w + s->crop_l, 1, W, o);
            for (int p = 1; p < 3; p++)
                for (uint32_t y = 0; y < (H + 1) / 2; y++)
                    fwrite(h.out->plane[p] + (size_t)(y + s->crop_t / 2) * h.cw + s->crop_l / 2, 1, (W + 1) / 2, o);
            frames++;
        } else if (h264_error(&h)[0]) {
            fprintf(stderr, "frame %d: %s\n", frames, h264_error(&h));
            return 1;
        }
        start = i;
    }
    fclose(o);
    printf("%d frames\n", frames);
    return 0;
}
