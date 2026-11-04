// Host tests for the .img / .vid codec (Minimal-OS-SDK/include/mosmedia.h).
// Build and run: make test-host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mosmedia.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static uint32_t rng = 1;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8 ^ rng << 13; }

static void test_swar(void) {
    for (int i = 0; i < 100000; i++) {
        uint32_t a = rnd() * 2654435761u, b = rnd() * 40503u + rnd();
        uint32_t d = mm_sub(a, b), ok = 1;
        for (int c = 0; c < 4; c++)
            ok &= ((d >> (8 * c)) & 255u) == (((a >> (8 * c)) - (b >> (8 * c))) & 255u);
        CHECK(ok, "mm_sub per byte");
        CHECK(mm_add(d, b) == a, "mm_add undoes mm_sub");
        if (!ok) return;
    }
}

// Pictures of different kinds: noise, gradients, flat with shapes.
static void make_picture(uint32_t* px, uint32_t w, uint32_t h, uint32_t stride, int kind, bool alpha) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c;
            if (kind == 0) c = rnd();
            else if (kind == 1) c = (x * 255 / (w ? w : 1)) << 16 | (y * 255 / (h ? h : 1)) << 8 | ((x + y) & 255);
            else c = ((x / 7 + y / 5) % 3 == 0) ? 0x3366CC : (x > w / 2 ? 0xF0F0F0 : 0x202020);
            uint32_t a = alpha ? ((kind == 0) ? rnd() & 255u : (x * 7 + y) & 255u) : 255u;
            px[(size_t)y * stride + x] = a << 24 | (c & 0xFFFFFFu);
        }
}

static void test_image(uint32_t w, uint32_t h, int kind, bool alpha) {
    uint32_t stride = w + 3;
    uint32_t* px = calloc((size_t)stride * h, 4);
    make_picture(px, w, h, stride, kind, alpha);
    if (!alpha) for (size_t i = 0; i < (size_t)stride * h; i++) px[i] &= 0x00FFFFFFu;   // alpha ignored
    size_t cap = mimg_encode_bound(w, h);
    uint8_t* enc = malloc(cap);
    size_t n = mimg_encode(px, w, h, stride, alpha, enc, cap);
    CHECK(n > 0 && n <= cap, "encodes");
    mimg_info_t info;
    CHECK(mimg_info(enc, n, &info) && info.width == w && info.height == h && info.alpha == alpha, "info");
    uint32_t* out = calloc((size_t)w * h, 4);
    CHECK(mimg_decode(enc, n, out, w), "decodes");
    int same = 1;
    for (uint32_t y = 0; y < h && same; y++)
        for (uint32_t x = 0; x < w && same; x++) {
            uint32_t want = px[(size_t)y * stride + x] | (alpha ? 0 : 0xFF000000u);
            same = out[(size_t)y * w + x] == want;
        }
    if (!same) printf("  image %ux%u kind %d alpha %d differs\n", w, h, kind, alpha);
    CHECK(same, "image round trip is exact");
    // Truncated and damaged copies must fail cleanly (ASan checks the rest).
    CHECK(!mimg_decode(enc, n / 2 < MIMG_HEADER_SIZE ? MIMG_HEADER_SIZE - 1 : n / 2, out, w) || n / 2 >= n,
          "truncated image rejected");
    for (int trial = 0; trial < 50; trial++) {
        uint8_t* bad = malloc(n);
        memcpy(bad, enc, n);
        for (int k = 0; k < 4; k++) bad[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
        mimg_info_t bi;   // a damaged header may name another size: size the buffer by it
        if (mimg_info(bad, n, &bi)) {
            uint32_t* tmp = malloc((size_t)bi.width * bi.height * 4);
            if (tmp) mimg_decode(bad, n, tmp, bi.width);
            free(tmp);
        }
        free(bad);
    }
    free(px); free(enc); free(out);
}

// In-memory file for the video writer and reader.
typedef struct { uint8_t* data; size_t size, cap; } membuf_t;
static bool mem_write(void* ctx, uint64_t off, const void* d, size_t n) {
    membuf_t* m = ctx;
    if (off + n > m->cap) {
        size_t cap = (off + n) * 2;
        m->data = realloc(m->data, cap);
        memset(m->data + m->cap, 0, cap - m->cap);
        m->cap = cap;
    }
    memcpy(m->data + off, d, n);
    if (off + n > m->size) m->size = off + n;
    return true;
}
static bool mem_read(void* ctx, uint64_t off, void* d, size_t n) {
    membuf_t* m = ctx;
    if (off > m->size || n > m->size - off) return false;
    memcpy(d, m->data + off, n);
    return true;
}

static void video_frame(uint32_t* px, uint32_t w, uint32_t h, uint32_t stride, uint32_t t) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = 0xFF000000u | (x * 3) << 16 | (y * 2) << 8 | 0x40;
            // a moving box, a ticking counter area, some noise in one corner
            if (x >= t * 5 % w && x < t * 5 % w + 30 && y >= 20 && y < 60) c = 0xFFFF2020u;
            if (x < 16 && y < 16 && t % 3 == 0) c = 0xFF000000u | rnd();
            px[(size_t)y * stride + x] = c;
        }
}

static void test_video(uint32_t w, uint32_t h, bool fast) {
    membuf_t m = {0};
    mvid_writer_t v;
    CHECK(mvid_writer_begin(&v, w, h, 30, 1, false, 10, fast, mem_write, &m, malloc, free), "begin");
    uint32_t frames = 37, stride = w + 5;
    uint32_t* px = malloc((size_t)stride * h * 4);
    uint32_t* all = malloc((size_t)w * h * 4 * frames);
    for (uint32_t t = 0; t < frames; t++) {
        video_frame(px, w, h, stride, t);
        for (uint32_t y = 0; y < h; y++) memcpy(all + ((size_t)t * h + y) * w, px + (size_t)y * stride, w * 4);
        CHECK(mvid_writer_frame(&v, px, stride), "frame");
    }
    static const char adi[] = "AudioFormat=PCM16\nSampleRate=8000\nChannels=1\n#DATA\n\x01\x02\x03\x04";
    CHECK(mvid_writer_finish(&v, adi, sizeof(adi) - 1), "finish");

    mvid_reader_t r;
    CHECK(mvid_open(&r, mem_read, &m, m.size, malloc), "open");
    CHECK(r.info.frame_count == frames && r.info.width == w && r.info.has_audio, "video info");
    CHECK(r.info.duration_ms == frames * 1000 / 30, "duration");
    CHECK(r.info.audio_size == sizeof(adi) - 1 && !memcmp(m.data + r.info.audio_offset, adi, sizeof(adi) - 1),
          "soundtrack stored unchanged");
    uint32_t* out = malloc((size_t)w * h * 4);
    // In order, then jumping around (backwards, across keyframes).
    uint32_t order[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 25, 26, 3, 36, 35, 9, 10, 0, 20, 19, 21};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        uint32_t n = order[i];
        bool ok = mvid_decode(&r, n, out);
        CHECK(ok, "decode frame");
        if (ok && memcmp(out, all + (size_t)n * w * h, (size_t)w * h * 4)) {
            printf("  frame %u differs (%ux%u fast %d)\n", n, w, h, fast);
            failures++;
            break;
        }
    }
    CHECK(mvid_frame_at(&r.info, 0) == 0 && mvid_frame_at(&r.info, 100) == 3 &&
          mvid_frame_at(&r.info, 999999) == frames - 1, "frame at time");
    // Damaged frames: no crash, and decoding recovers at the next keyframe.
    for (int trial = 0; trial < 40; trial++) {
        uint8_t keep[64];
        size_t at = MVID_HEADER_SIZE + rnd() % (r.info.index_offset - MVID_HEADER_SIZE - 64);
        memcpy(keep, m.data + at, 64);
        for (int k = 0; k < 3; k++) m.data[at + rnd() % 64] ^= (uint8_t)(1u << (rnd() % 8));
        mvid_decode(&r, rnd() % frames, out);
        memcpy(m.data + at, keep, 64);
    }
    r.decoded = -1;
    CHECK(mvid_decode(&r, 30, out) && !memcmp(out, all + (size_t)30 * w * h, (size_t)w * h * 4),
          "decodes again after damage");
    // Tiny file and wrong magic.
    mvid_reader_t bad;
    CHECK(!mvid_open(&bad, mem_read, &m, 10, malloc), "short file rejected");
    mvid_close(&bad, free);
    mvid_close(&r, free);
    printf("  video %ux%u%s: %u frames, %zu bytes (raw %zu)\n", w, h, fast ? " fast" : "", frames, m.size,
           (size_t)w * h * 4 * frames);
    free(px); free(all); free(out); free(m.data);
}

int main(void) {
    test_swar();
    uint32_t sizes[][2] = {{1, 1}, {64, 64}, {65, 3}, {3, 130}, {130, 70}, {200, 129}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (int kind = 0; kind < 3; kind++)
            for (int alpha = 0; alpha < 2; alpha++) test_image(sizes[i][0], sizes[i][1], kind, alpha);
    test_video(160, 90, false);
    test_video(160, 90, true);
    test_video(67, 129, false);
    if (failures) {
        printf("media tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("media tests passed\n");
    return 0;
}
