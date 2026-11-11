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

static void video_frame(uint32_t* px, uint32_t w, uint32_t h, uint32_t stride, uint32_t t, bool alpha) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t a = alpha ? (x * 7u + y * 11u + t * 13u) & 255u : 255u;
            uint32_t c = a << 24 | (x * 3) << 16 | (y * 2) << 8 | 0x40;
            // a moving box, a ticking counter area, some noise in one corner
            if (x >= t * 5 % w && x < t * 5 % w + 30 && y >= 20 && y < 60) c = a << 24 | 0x00FF2020u;
            if (x < 16 && y < 16 && t % 3 == 0) c = alpha ? rnd() : 0xFF000000u | rnd();
            px[(size_t)y * stride + x] = c;
        }
}

static void test_video(uint32_t w, uint32_t h, bool fast, bool alpha) {
    membuf_t m = {0};
    mvid_writer_t v;
    CHECK(mvid_writer_begin(&v, w, h, 30, 1, alpha, 10, fast, mem_write, &m, malloc, free), "begin");
    uint32_t frames = 37, stride = w + 5;
    uint32_t* px = malloc((size_t)stride * h * 4);
    uint32_t* all = malloc((size_t)w * h * 4 * frames);
    for (uint32_t t = 0; t < frames; t++) {
        video_frame(px, w, h, stride, t, alpha);
        for (uint32_t y = 0; y < h; y++) memcpy(all + ((size_t)t * h + y) * w, px + (size_t)y * stride, w * 4);
        CHECK(mvid_writer_frame(&v, px, stride), "frame");
    }
    static const char adi[] = "AudioFormat=PCM16\nSampleRate=8000\nChannels=1\n#DATA\n\x01\x02\x03\x04";
    CHECK(mvid_writer_finish(&v, adi, sizeof(adi) - 1), "finish");

    mvid_reader_t r;
    CHECK(mvid_open(&r, mem_read, &m, m.size, malloc), "open");
    CHECK(r.info.frame_count == frames && r.info.width == w && r.info.has_audio &&
          r.info.alpha == alpha, "video info");
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

// Lossy videos: every decoded pixel is within `loss` of the source (alpha
// exact), also when seeking, and noisy footage gets smaller.
static size_t encode_noisy(membuf_t* m, uint32_t w, uint32_t h, uint32_t frames, bool alpha,
                           uint32_t loss, bool fast, uint32_t* all) {
    mvid_writer_t v;
    CHECK(mvid_writer_begin(&v, w, h, 30, 1, alpha, 10, fast, mem_write, m, malloc, free), "begin");
    v.loss = loss;
    rng = 7;
    for (uint32_t t = 0; t < frames; t++) {
        uint32_t* px = all + (size_t)t * w * h;
        video_frame(px, w, h, w, t, alpha);
        for (size_t i = 0; i < (size_t)w * h; i++) {   // camera-like noise, and edge values
            uint32_t c = px[i], out = c & 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8) {
                int ch = (int)((c >> sh) & 255u) + (int)(rnd() % 9u) - 4;
                if (i % 97 == 0) ch = sh ? 0 : 255;
                out |= (uint32_t)(ch < 0 ? 0 : ch > 255 ? 255 : ch) << sh;
            }
            px[i] = out;
        }
        CHECK(mvid_writer_frame(&v, px, w), "frame");
    }
    CHECK(mvid_writer_finish(&v, NULL, 0), "finish");
    return m->size;
}

static void test_video_lossy(uint32_t w, uint32_t h, bool alpha, uint32_t loss, bool fast) {
    uint32_t frames = 25;
    uint32_t* all = malloc((size_t)w * h * 4 * frames);
    membuf_t exact = {0}, m = {0};
    size_t lossless = encode_noisy(&exact, w, h, frames, alpha, 0, fast, all);
    size_t lossy = encode_noisy(&m, w, h, frames, alpha, loss, fast, all);
    mvid_reader_t r;
    CHECK(mvid_open(&r, mem_read, &m, m.size, malloc), "open");
    uint32_t* out = malloc((size_t)w * h * 4);
    uint32_t order[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                        13, 4, 22};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        uint32_t n = order[i];
        CHECK(mvid_decode(&r, n, out), "decode lossy frame");
        const uint32_t* src = all + (size_t)n * w * h;
        size_t bad = 0;
        for (size_t p = 0; p < (size_t)w * h; p++)
            bad += !mm_close(out[p], src[p] | (alpha ? 0 : 0xFF000000u), loss);
        if (bad) { printf("  lossy frame %u: %zu pixels too far\n", n, bad); failures++; break; }
    }
    CHECK(lossy < lossless, "lossy is smaller");
    printf("  lossy video %ux%u loss %u%s%s: %zu bytes (lossless %zu)\n", w, h, loss, alpha ? " alpha" : "",
           fast ? " fast" : "", lossy, lossless);
    mvid_close(&r, free);
    free(all); free(out); free(m.data); free(exact.data);
}

// The motion codec: decoders show exactly the encoder's reference frames
// (also when seeking), quality is sane, damage fails cleanly.
static void test_video_motion(uint32_t w, uint32_t h, uint32_t qp) {
    uint32_t frames = 31;
    membuf_t m = {0};
    mvid_writer_t v;
    CHECK(mvid_writer_begin(&v, w, h, 30, 1, true, 10, false, mem_write, &m, malloc, free), "begin");
    v.codec = MVID_CODEC_MOTION;
    v.qp = qp;
    uint32_t* src = malloc((size_t)w * h * 4 * frames);
    uint32_t* rec = malloc((size_t)w * h * 4 * frames);
    rng = 3;
    for (uint32_t t = 0; t < frames; t++) {
        uint32_t* px = src + (size_t)t * w * h;
        video_frame(px, w, h, w, t, false);
        for (size_t i = 0; i < (size_t)w * h; i++) {   // a little noise, like a camera
            uint32_t c = px[i], out = 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8) {
                int ch = (int)((c >> sh) & 255u) + (int)(rnd() % 5u) - 2;
                out |= (uint32_t)(ch < 0 ? 0 : ch > 255 ? 255 : ch) << sh;
            }
            px[i] = out;
        }
        CHECK(mvid_writer_frame(&v, px, w), "motion frame");
        mmc_to_argb(&v.mc, rec + (size_t)t * w * h, w, h, w);
    }
    static const char adi[] = "AudioFormat=PCM16\nSampleRate=8000\nChannels=1\n#DATA\n\x01\x02";
    CHECK(mvid_writer_finish(&v, adi, sizeof(adi) - 1), "finish");
    mvid_reader_t r;
    CHECK(mvid_open(&r, mem_read, &m, m.size, malloc), "open motion");
    CHECK(r.info.codec == MVID_CODEC_MOTION && !r.info.alpha && r.info.frame_count == frames && r.info.has_audio,
          "motion info");
    uint32_t* out = malloc((size_t)w * h * 4);
    uint32_t order[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 25, 26, 3, 30, 29, 9, 10, 0, 20, 19, 21};
    double err = 0;
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        uint32_t n = order[i];
        CHECK(mvid_decode(&r, n, out), "decode motion frame");
        if (memcmp(out, rec + (size_t)n * w * h, (size_t)w * h * 4)) {
            printf("  motion frame %u differs from the encoder's (%ux%u)\n", n, w, h);
            failures++;
            break;
        }
        // (Random colour noise in the corner is beyond 4:2:0 colour; leave it out.)
        for (size_t p = 0; p < (size_t)w * h; p++)
            for (int sh = 0; sh < 24 && (p % w >= 16 || p / w >= 16); sh += 8) {
                double d = (double)((out[p] >> sh) & 255u) - (double)((src[(size_t)n * w * h + p] >> sh) & 255u);
                err += d * d;
            }
    }
    double counted = (double)w * h - (double)(w < 16 ? w : 16) * (h < 16 ? h : 16);
    double mse = counted > 0 ? err / (counted * 3 * (sizeof(order) / sizeof(order[0]))) : 0;
    if (qp <= 30) CHECK(mse < 65.0, "motion quality (better than 30 dB PSNR)");
    // Damaged frames: no crash, and decoding recovers at the next keyframe.
    for (int trial = 0; trial < 60; trial++) {
        uint8_t keep[32];
        size_t at = MVID_HEADER_SIZE + rnd() % (r.info.index_offset - MVID_HEADER_SIZE - 32);
        memcpy(keep, m.data + at, 32);
        for (int k = 0; k < 3; k++) m.data[at + rnd() % 32] ^= (uint8_t)(1u << (rnd() % 8));
        r.decoded = -1;
        mvid_decode(&r, rnd() % frames, out);
        memcpy(m.data + at, keep, 32);
    }
    r.decoded = -1;
    CHECK(mvid_decode(&r, 22, out) && !memcmp(out, rec + (size_t)22 * w * h, (size_t)w * h * 4),
          "motion decodes again after damage");
    printf("  motion video %ux%u q %u: %zu bytes, mean squared error %.1f\n", w, h, qp, m.size, mse);
    mvid_close(&r, free);
    free(src); free(rec); free(out); free(m.data);
}

// Macroblocks in another valid order (anti-diagonals, the order threads
// may finish them in) must give the same bytes as in order.
static void diagonal_order(void* ctx, mmc_t* m, const mmc_job_t* job) {
    (void)ctx;
    for (uint32_t d = 0; d < m->mbw + 2 * m->mbh; d++)
        for (uint32_t my = 0; my < m->mbh; my++) {
            if (d < 2 * my) break;
            uint32_t mx = d - 2 * my;
            if (mx < m->mbw) mmc_encode_mb(m, job, mx, my);
        }
}

static void test_motion_order(void) {
    uint32_t w = 150, h = 100, frames = 12;
    membuf_t a = {0}, b = {0};
    uint32_t* px = malloc((size_t)w * h * 4);
    for (int pass = 0; pass < 2; pass++) {
        mvid_writer_t v;
        CHECK(mvid_writer_begin(&v, w, h, 30, 1, false, 6, false, mem_write, pass ? &b : &a, malloc, free), "begin");
        v.codec = MVID_CODEC_MOTION;
        if (pass) v.parallel = diagonal_order;
        rng = 11;
        for (uint32_t t = 0; t < frames; t++) {
            video_frame(px, w, h, w, t, false);
            CHECK(mvid_writer_frame(&v, px, w), "frame");
        }
        CHECK(mvid_writer_finish(&v, NULL, 0), "finish");
    }
    CHECK(a.size == b.size && !memcmp(a.data, b.data, a.size), "motion output does not depend on macroblock order");
    free(px); free(a.data); free(b.data);
}

/* ------------------------------------------------------------- H.264 */

// The decoded planes of every frame of docs/examples/sample-h264.vid,
// hashed (FNV-1a) as ffmpeg decodes them: H.264 decoding is exact, so the
// decoder here must reach the same number. tools/media/h264_check.sh
// compares many more streams when ffmpeg is installed.
#define SAMPLE_H264 "docs/examples/sample-h264.vid"
#define SAMPLE_H264_HASH 0x5429ff1e233bf1b8ull

static uint64_t hash_planes(const h264_t* d, uint64_t hash) {
    const h264_sps_t* s = d->active;
    uint32_t w = h264_width(d), h = h264_height(d);
    for (int p = 0; p < 3; p++) {
        uint32_t pw = p ? (w + 1) / 2 : w, ph = p ? (h + 1) / 2 : h, stride = p ? d->cw : d->w;
        const uint8_t* base = d->out->plane[p] + (size_t)(p ? s->crop_t / 2 : s->crop_t) * stride +
                              (p ? s->crop_l / 2 : s->crop_l);
        for (uint32_t y = 0; y < ph; y++)
            for (uint32_t x = 0; x < pw; x++) hash = (hash ^ base[(size_t)y * stride + x]) * 0x100000001b3ull;
    }
    return hash;
}

static void test_h264(void) {
    membuf_t m = {0};
    FILE* f = fopen(SAMPLE_H264, "rb");
    CHECK(f != NULL, "sample H.264 video is there (run from the repository root)");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    m.size = m.cap = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    m.data = malloc(m.size);
    CHECK(fread(m.data, 1, m.size, f) == m.size, "sample read");
    fclose(f);

    mvid_reader_t r;
    CHECK(mvid_open(&r, mem_read, &m, m.size, malloc), "h264 video opens");
    CHECK(r.info.codec == MVID_CODEC_H264 && r.info.width == 160 && r.info.height == 90 &&
          r.info.frame_count == 24 && !r.info.alpha, "h264 video header");
    size_t px = (size_t)r.info.width * r.info.height;
    uint32_t* out = malloc(px * 4);
    uint32_t* frame20 = malloc(px * 4);
    uint32_t* again = malloc(px * 4);
    uint64_t hash = 0xcbf29ce484222325ull;
    bool ok = true;
    for (uint32_t n = 0; n < r.info.frame_count && ok; n++) {
        ok = mvid_decode(&r, n, out);
        if (!ok) break;
        hash = hash_planes(&r.h264, hash);
        if (n == 20) memcpy(frame20, out, px * 4);
        if (n == 0) {
            // The pixels are the decoder's own conversion, opaque.
            h264_to_argb(&r.h264, again, r.info.width, r.info.height, r.info.width);
            CHECK(!memcmp(out, again, px * 4) && (out[0] >> 24) == 0xFF, "h264 pixels");
            CHECK(h264_is_full_601(&r.h264), "the converter writes full-range BT.601, like the motion codec");
        }
    }
    CHECK(ok, "every h264 frame decodes");
    CHECK(hash == SAMPLE_H264_HASH, "h264 frames are exactly what ffmpeg decodes");
    // Seeking: back to the start, then straight to frame 20 (from its keyframe).
    CHECK(mvid_decode(&r, 3, out) && mvid_decode(&r, 20, out) && !memcmp(out, frame20, px * 4), "h264 seeking");
    mvid_close(&r, free);

    // Damaged copies must fail cleanly or decode something: never crash
    // or read outside their buffers (ASan checks that).
    uint64_t index_at = mm_get64(m.data + 32);
    membuf_t bad = {malloc(m.size), m.size, m.size};
    int decoded = 0;
    for (int round = 0; round < 400; round++) {
        memcpy(bad.data, m.data, m.size);
        int hits = 1 + (int)(rnd() % 3u);
        for (int k = 0; k < hits; k++) {
            size_t at = 64 + rnd() % (index_at - 64);
            if (round & 1) bad.data[at] ^= (uint8_t)(1u << (rnd() % 8u));
            else bad.data[at] = (uint8_t)rnd();
        }
        mvid_reader_t b;
        if (mvid_open(&b, mem_read, &bad, bad.size, malloc))
            for (uint32_t n = 0; n < b.info.frame_count; n++) decoded += mvid_decode(&b, n, out);
        mvid_close(&b, free);
    }
    CHECK(decoded > 0, "damaged h264 videos still decode where they can");

    // What the decoder does not do, it says.
    h264_t d;
    CHECK(h264_init(&d, malloc, 64), "decoder init");
    static const uint8_t cabac_pps[] = { 0, 0, 1, 0x68, 0xE0, 0x80 };      // entropy_coding_mode_flag = 1
    CHECK(!h264_decode(&d, cabac_pps, sizeof cabac_pps) && strstr(h264_error(&d), "CABAC"), "CABAC is refused by name");
    static const uint8_t slice_only[] = { 0, 0, 1, 0x65, 0x88, 0x80, 0x40 };
    CHECK(!h264_decode(&d, slice_only, sizeof slice_only) && h264_error(&d)[0], "a slice without parameter sets fails");
    h264_free(&d, free);
    free(out); free(frame20); free(again); free(bad.data); free(m.data);
}

/* A stand-in for the graphics card's H.264 decoder: checks what it is
 * told about each picture and answers with the right pixels (from a CPU
 * decoder on the same file), unless asked to fail or to be wrong. */
typedef struct {
    mvid_reader_t cpu;
    uint32_t n;                 // the frame being decoded
    uint32_t calls, mismatches;
    int fail_at, wrong_at;      // frame numbers, -1 = never
    uint32_t seen;              // picture buffers written since the last keyframe
    bool described;             // every picture description was sound
} fake_hw_t;

static bool fake_hw_decode(void* ctx, const syscall_media_h264_t* pic, uint32_t* out, uint32_t w, uint32_t h) {
    fake_hw_t* f = ctx;
    f->calls++;
    if ((int)f->n == f->fail_at) return false;
    bool ok = pic->mb_w == 10 && pic->mb_h == 6 && pic->slice_count >= 1 && pic->data_bytes > 4 &&
              pic->cur_index < SYS_MEDIA_H264_SURFACES && pic->entropy_coding_mode == 0 && w == 160 && h == 90;
    if (pic->flags & SYS_MEDIA_H264_RESET) { f->seen = 0; ok = ok && pic->ref_count == 0; }
    else ok = ok && pic->ref_count >= 1;
    for (uint32_t i = 0; i < pic->ref_count; i++)     // references are pictures decoded before
        ok = ok && pic->refs[i].index != pic->cur_index && (f->seen & (1u << pic->refs[i].index));
    for (uint32_t i = 0; i < pic->slice_count; i++) {
        const uint8_t* p = pic->data + pic->slice_offset[i];
        int type = p[3] & 31;
        ok = ok && pic->slice_offset[i] + 4 <= pic->data_bytes && p[0] == 0 && p[1] == 0 && p[2] == 1 &&
             (type == 1 || type == 5);
    }
    f->seen |= 1u << pic->cur_index;
    f->described = f->described && ok;
    if (!mvid_decode(&f->cpu, f->n, out)) return false;
    if ((int)f->n == f->wrong_at) out[w * 40 + 80] ^= 0x00010000u;
    return true;
}

static void fake_hw_mismatch(void* ctx) { ((fake_hw_t*)ctx)->mismatches++; }

// Runs the sample through a reader with the stand-in attached, in the
// order `frames` gives, and checks every picture against `want`.
static bool hw_run_more(membuf_t* m, const uint32_t* want, const uint32_t* frames, int count, fake_hw_t* f,
                        mvid_reader_t* r) {
    (void)m;
    size_t px = 160 * 90;
    uint32_t* out = malloc(px * 4);
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        // The reader decodes from a keyframe up to the frame wanted: tell
        // the stand-in which frame each call is for.
        uint32_t n = frames[i], from;
        if (r->decoded >= 0 && (int64_t)n > r->decoded) {
            from = (uint32_t)r->decoded + 1;
            for (uint32_t k = n; k > from; k--) if (k % 12 == 0) { from = k; break; }
        } else {
            from = n - n % 12;
        }
        for (uint32_t k = from; k <= n && ok; k++) {
            f->n = k;
            ok = mvid_decode(r, k, out) && !memcmp(out, want + (size_t)k * px, px * 4);
        }
    }
    free(out);
    return ok;
}

static bool hw_run(membuf_t* m, const uint32_t* want, const uint32_t* frames, int count, fake_hw_t* f,
                   mvid_reader_t* r) {
    bool ok = mvid_open(r, mem_read, m, m->size, malloc) && mvid_open(&f->cpu, mem_read, m, m->size, malloc);
    r->hw_decode = fake_hw_decode;
    r->hw_mismatch = fake_hw_mismatch;
    r->hw_ctx = f;
    f->described = true;
    return ok && hw_run_more(m, want, frames, count, f, r);
}

static void test_h264_hardware(void) {
    membuf_t m = {0};
    FILE* file = fopen(SAMPLE_H264, "rb");
    if (!file) return;          // test_h264 has reported it
    fseek(file, 0, SEEK_END);
    m.size = m.cap = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);
    m.data = malloc(m.size);
    CHECK(fread(m.data, 1, m.size, file) == m.size, "sample read");
    fclose(file);
    size_t px = 160 * 90;
    uint32_t* want = malloc(24 * px * 4);
    mvid_reader_t plain;
    CHECK(mvid_open(&plain, mem_read, &m, m.size, malloc), "open");
    for (uint32_t n = 0; n < 24; n++) CHECK(mvid_decode(&plain, n, want + n * px), "reference frame");
    mvid_close(&plain, free);
    uint32_t all[24];
    for (uint32_t i = 0; i < 24; i++) all[i] = i;

    // A decoder that is right: checked over the first group of pictures,
    // on its own from the second keyframe.
    fake_hw_t f = { .fail_at = -1, .wrong_at = -1 };
    mvid_reader_t r;
    CHECK(hw_run(&m, want, all, 24, &f, &r), "hardware decoder: every picture is right");
    CHECK(r.hw_state == MVID_HW_ONLY && f.calls == 24 && f.mismatches == 0 && r.hw_checked == MVID_HW_CHECK_FRAMES,
          "hardware decoder takes over after the checked pictures");
    CHECK(f.described, "pictures are described completely to the hardware decoder");
    CHECK(r.h264.hw_only, "after the hand-over the CPU reads headers only");
    // Seeking backwards after the hand-over: from the keyframe, on the hardware decoder.
    uint32_t seek[3] = { 23, 14, 20 };
    CHECK(hw_run_more(&m, want, seek, 3, &f, &r) && r.hw_state == MVID_HW_ONLY, "seeking on the hardware decoder");
    mvid_close(&r, free); mvid_close(&f.cpu, free);

    // One wrong pixel while it is being checked: reported, and never used.
    fake_hw_t bad = { .fail_at = -1, .wrong_at = 5 };
    CHECK(hw_run(&m, want, all, 24, &bad, &r), "a wrong hardware decoder never reaches the screen");
    CHECK(r.hw_state == MVID_HW_OFF && bad.mismatches == 1 && bad.calls == 6, "it is reported and switched off");
    mvid_close(&r, free); mvid_close(&bad.cpu, free);

    // It gives up after taking over: the CPU decoder catches up from the keyframe.
    fake_hw_t quit = { .fail_at = 17, .wrong_at = -1 };
    CHECK(hw_run(&m, want, all, 24, &quit, &r), "a hardware decoder that gives up is replaced mid-stream");
    CHECK(r.hw_state == MVID_HW_OFF && !r.h264.hw_only && quit.mismatches == 0, "the file continues on the CPU");
    mvid_close(&r, free); mvid_close(&quit.cpu, free);

    // It refuses from the start (no such hardware): nothing changes.
    fake_hw_t none = { .fail_at = 0, .wrong_at = -1 };
    CHECK(hw_run(&m, want, all, 24, &none, &r) && r.hw_state == MVID_HW_OFF && none.calls == 1,
          "without a hardware decoder it is asked once");
    mvid_close(&r, free); mvid_close(&none.cpu, free);
    free(want); free(m.data);
}

int main(void) {
    test_swar();
    uint32_t sizes[][2] = {{1, 1}, {64, 64}, {65, 3}, {3, 130}, {130, 70}, {200, 129}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (int kind = 0; kind < 3; kind++)
            for (int alpha = 0; alpha < 2; alpha++) test_image(sizes[i][0], sizes[i][1], kind, alpha);
    test_video(160, 90, false, false);
    test_video(160, 90, true, false);
    test_video(67, 129, false, false);
    test_video(67, 129, false, true);
    test_video_lossy(160, 90, false, 4, false);
    test_video_lossy(160, 90, false, 8, true);
    test_video_lossy(67, 129, true, 6, false);
    test_video_lossy(65, 70, false, 255, false);
    test_video_motion(160, 90, 26);
    test_video_motion(67, 129, 20);
    test_video_motion(1, 1, 30);
    test_video_motion(33, 17, 0);
    test_video_motion(48, 32, 51);
    test_motion_order();
    test_h264();
    test_h264_hardware();
    if (failures) {
        printf("media tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("media tests passed\n");
    return 0;
}
