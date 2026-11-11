/*
 * mosmedia: makes and checks Minimal-OS .img images and .vid videos on the
 * build PC (ffmpeg reads and writes the other formats).
 *
 *   mosmedia img   <picture> <out.img>        PNG, JPEG, ... -> .img (keeps transparency)
 *   mosmedia png   <in.img> <out.png>         .img -> PNG
 *   mosmedia vid   <video> <out.vid> [options] MP4, MKV, GIF, ... -> .vid with sound
 *         --width N    scale to N pixels wide (keeps the shape)
 *         --fps N      frames per second (default: the source's, at most 60)
 *         --key N      a keyframe every N frames (default 2 seconds)
 *         --no-audio   leave the sound out
 *         --fast       quicker, a little larger
 *         --codec C    motion (default: lossy, H.264-like, for camera footage),
 *                      tiles (lossless, for screens and drawings) or
 *                      h264 (H.264 Constrained Baseline, encoded by x264)
 *         --q N        motion: quality, 0 (best) to 51, default 26;
 *                      h264: x264's quality (crf), the same scale
 *         --loss N     tiles: colours may move up to N (0-255, default 0 =
 *                      lossless)
 *         --threads N  motion: encoder threads (default: one per CPU core)
 *         --no-hwaccel decode the input on the CPU only (by default ffmpeg
 *                      uses the GPU's video decoder when it has one)
 *   mosmedia unvid <in.vid> <out.mp4>         .vid -> video file (to check one;
 *                                             out.raw = the exact pixels, BGRA)
 *   mosmedia info  <file.img | file.vid>
 *
 * Build: make build/tools/mosmedia (needs ffmpeg and ffprobe). Builds on
 * Linux, macOS and Windows (MinGW-w64: x86_64-w64-mingw32-gcc -O2
 * -idirafter Minimal-OS-SDK/include tools/media/mosmedia.c -o mosmedia.exe).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "mosmedia.h"

/* ------------------------------------------------- platform: Windows/POSIX */
#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#define PIPE_READ "rb"
#define PIPE_WRITE "wb"
#define DEVNULL "NUL"
#define fseek64 _fseeki64
#define ftell64 _ftelli64
typedef HANDLE thread_t;
typedef CRITICAL_SECTION mutex_t;
typedef CONDITION_VARIABLE cond_t;
static DWORD WINAPI thread_trampoline(LPVOID arg) {
    void** a = (void**)arg;
    ((void (*)(void*))a[0])(a[1]);
    free(a);
    return 0;
}
static bool thread_start(thread_t* t, void (*fn)(void*), void* arg) {
    void** a = malloc(2 * sizeof(void*));
    if (!a) return false;
    a[0] = (void*)fn; a[1] = arg;
    *t = CreateThread(NULL, 0, thread_trampoline, a, 0, NULL);
    return *t != NULL;
}
static void thread_join(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static void thread_yield(void) { SwitchToThread(); }
static void mutex_init(mutex_t* m) { InitializeCriticalSection(m); }
static void mutex_lock(mutex_t* m) { EnterCriticalSection(m); }
static void mutex_unlock(mutex_t* m) { LeaveCriticalSection(m); }
static void cond_init(cond_t* c) { InitializeConditionVariable(c); }
static void cond_wait(cond_t* c, mutex_t* m) { SleepConditionVariableCS(c, m, INFINITE); }
static void cond_broadcast(cond_t* c) { WakeAllConditionVariable(c); }
static int cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#define PIPE_READ "r"
#define PIPE_WRITE "w"
#define DEVNULL "/dev/null"
#define fseek64 fseeko
#define ftell64 ftello
typedef pthread_t thread_t;
typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t cond_t;
typedef struct { void (*fn)(void*); void* arg; } thread_arg_t;
static void* thread_trampoline(void* arg) {
    thread_arg_t a = *(thread_arg_t*)arg;
    free(arg);
    a.fn(a.arg);
    return NULL;
}
static bool thread_start(thread_t* t, void (*fn)(void*), void* arg) {
    thread_arg_t* a = malloc(sizeof *a);
    if (!a) return false;
    a->fn = fn; a->arg = arg;
    if (pthread_create(t, NULL, thread_trampoline, a) != 0) { free(a); return false; }
    return true;
}
static void thread_join(thread_t t) { pthread_join(t, NULL); }
static void thread_yield(void) { sched_yield(); }
static void mutex_init(mutex_t* m) { pthread_mutex_init(m, NULL); }
static void mutex_lock(mutex_t* m) { pthread_mutex_lock(m); }
static void mutex_unlock(mutex_t* m) { pthread_mutex_unlock(m); }
static void cond_init(cond_t* c) { pthread_cond_init(c, NULL); }
static void cond_wait(cond_t* c, mutex_t* m) { pthread_cond_wait(c, m); }
static void cond_broadcast(cond_t* c) { pthread_cond_broadcast(c); }
static int cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

static void die(const char* msg, const char* detail) {
    fprintf(stderr, "mosmedia: %s%s%s\n", msg, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

// `s` quoted for the shell (sh: single quotes; Windows cmd: double quotes,
// which file names cannot contain).
static const char* q(const char* s) {
    static char buf[8][4096];
    static int slot;
    char* out = buf[slot++ & 7];
    size_t n = 0;
#ifdef _WIN32
    out[n++] = '"';
    for (; *s && n + 2 < sizeof(buf[0]); s++) out[n++] = *s;
    out[n++] = '"';
#else
    out[n++] = '\'';
    for (; *s && n + 6 < sizeof(buf[0]); s++) {
        if (*s == '\'') { memcpy(out + n, "'\\''", 4); n += 4; }
        else out[n++] = *s;
    }
    out[n++] = '\'';
#endif
    out[n] = 0;
    return out;
}

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) die("cannot open", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc(n > 0 ? (size_t)n : 1);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) die("cannot read", path);
    fclose(f);
    *size = (size_t)n;
    return d;
}

// Width, height and frame rate of the first video stream.
static void probe(const char* path, uint32_t* w, uint32_t* h, uint32_t* fps_num, uint32_t* fps_den) {
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate "
             "-of csv=p=0 %s", q(path));
    FILE* p = popen(cmd, PIPE_READ);
    unsigned a = 0, b = 0, c = 0, d = 1;
    if (!p || fscanf(p, "%u,%u,%u/%u", &a, &b, &c, &d) < 2) die("ffprobe cannot read", path);
    pclose(p);
    *w = a; *h = b;
    if (fps_num) { *fps_num = c ? c : 30; *fps_den = d ? d : 1; }
}

static void cmd_img(const char* in, const char* out) {
    uint32_t w, h;
    probe(in, &w, &h, NULL, NULL);
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -i %s -frames:v 1 -f rawvideo -pix_fmt bgra -", q(in));
    FILE* p = popen(cmd, PIPE_READ);
    uint32_t* px = malloc((size_t)w * h * 4);
    if (!p || fread(px, 4, (size_t)w * h, p) != (size_t)w * h) die("ffmpeg cannot decode", in);
    pclose(p);
    bool alpha = false;
    for (size_t i = 0; i < (size_t)w * h && !alpha; i++) alpha = px[i] >> 24 != 255u;
    size_t cap = mimg_encode_bound(w, h);
    uint8_t* enc = malloc(cap);
    size_t n = mimg_encode(px, w, h, w, alpha, enc, cap);
    if (!n) die("cannot encode (too large?)", in);
    FILE* f = fopen(out, "wb");
    if (!f || fwrite(enc, 1, n, f) != n) die("cannot write", out);
    fclose(f);
    printf("%s: %ux%u %s, %zu bytes (%.1f%% of raw)\n", out, w, h, alpha ? "RGBA" : "RGB", n,
           100.0 * (double)n / ((double)w * h * (alpha ? 4 : 3)));
}

static void cmd_png(const char* in, const char* out) {
    size_t size;
    uint8_t* d = read_file(in, &size);
    mimg_info_t info;
    if (!mimg_info(d, size, &info)) die("not an .img file", in);
    uint32_t* px = malloc((size_t)info.width * info.height * 4);
    if (!mimg_decode(d, size, px, info.width)) die("damaged .img file", in);
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -y -f rawvideo -pix_fmt bgra -s %ux%u -i - -frames:v 1 %s",
             info.width, info.height, q(out));
    FILE* p = popen(cmd, PIPE_WRITE);
    if (!p || fwrite(px, 4, (size_t)info.width * info.height, p) != (size_t)info.width * info.height)
        die("ffmpeg failed", out);
    if (pclose(p) != 0) die("ffmpeg failed", out);
    printf("%s: %ux%u\n", out, info.width, info.height);
}

static bool file_write(void* ctx, uint64_t off, const void* d, size_t n) {
    FILE* f = ctx;
    return fseek64(f, (int64_t)off, SEEK_SET) == 0 && fwrite(d, 1, n, f) == n;
}
static bool file_read(void* ctx, uint64_t off, void* d, size_t n) {
    FILE* f = ctx;
    return fseek64(f, (int64_t)off, SEEK_SET) == 0 && fread(d, 1, n, f) == n;
}

// IMA ADPCM, as tools/audioconverter/wavtoadi.py writes it (and
// ima_decode_nibble() in src/modules/Audio/audio.c reads it).
static const int ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767};

typedef struct { int predictor, index; } ima_state_t;

static uint8_t ima_encode(ima_state_t* s, int sample) {
    int step = ima_step[s->index], diff = sample - s->predictor;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
    if (diff >= step >> 2) code |= 1;
    int delta = step >> 3;
    if (code & 1) delta += step >> 2;
    if (code & 2) delta += step >> 1;
    if (code & 4) delta += step;
    if (code & 8) delta = -delta;
    s->predictor += delta;
    if (s->predictor > 32767) s->predictor = 32767;
    if (s->predictor < -32768) s->predictor = -32768;
    s->index += ima_index[code & 7];
    if (s->index < 0) s->index = 0;
    if (s->index > 88) s->index = 88;
    return code;
}

// The source's sound as an .adi file (IMA ADPCM, 44.1 kHz stereo); NULL if it has none.
static uint8_t* make_soundtrack(const char* in, size_t* size) {
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -i %s -vn -ac 2 -ar 44100 -f s16le - 2>" DEVNULL, q(in));
    FILE* p = popen(cmd, PIPE_READ);
    if (!p) return NULL;
    size_t cap = 1u << 20, n = 0;           // ADPCM bytes
    uint8_t* adpcm = malloc(cap);
    ima_state_t left = {0, 0}, right = {0, 0};
    int16_t pcm[4096 * 4];                  // 2 stereo frames per 2 output bytes
    size_t got;
    uint64_t frames = 0;
    while ((got = fread(pcm, 8, 4096, p)) > 0) {
        if (n + got * 2 > cap) { cap = cap * 2 + got * 2; adpcm = realloc(adpcm, cap); }
        if (!adpcm) die("out of memory", NULL);
        for (size_t i = 0; i < got; i++) {
            const int16_t* f = pcm + i * 4;  // L0 R0 L1 R1
            adpcm[n++] = (uint8_t)(ima_encode(&left, f[0]) | ima_encode(&left, f[2]) << 4);
            adpcm[n++] = (uint8_t)(ima_encode(&right, f[1]) | ima_encode(&right, f[3]) << 4);
        }
        frames += got * 2;
    }
    pclose(p);
    if (!n) { free(adpcm); return NULL; }
    char header[512];
    int hn = snprintf(header, sizeof header,
                      "#Audio data generated by ADI converter\nAudioFormat=IADPCM\nAudioLength=%llu\n"
                      "AudioDatalen=%zu\nGlobalvol=80\nSampleRate=44100\nChannels=2\n#DATA\n",
                      (unsigned long long)(frames / 44100), n);
    uint8_t* adi = malloc((size_t)hn + n);
    if (!adi) die("out of memory", NULL);
    memcpy(adi, header, (size_t)hn);
    memcpy(adi + hn, adpcm, n);
    free(adpcm);
    *size = (size_t)hn + n;
    return adi;
}

/* ----------------------------------------------- motion encoder threads */

// Runs a frame's macroblocks on several threads as a wavefront: each row
// starts when the row above is two macroblocks ahead. Rows are handed out
// in order, so a thread only ever waits for a row that is being worked on.
typedef struct {
    int threads;
    mmc_t* m;
    const mmc_job_t* job;
    atomic_uint next_row;
    atomic_uint* done;          // per row: macroblocks finished
    uint32_t done_rows;
} wavefront_t;

static void wavefront_worker(void* arg) {
    wavefront_t* wf = arg;
    mmc_t* m = wf->m;
    for (;;) {
        uint32_t row = atomic_fetch_add(&wf->next_row, 1);
        if (row >= m->mbh) return;
        for (uint32_t mx = 0; mx < m->mbw; mx++) {
            if (row > 0) {
                uint32_t need = mx + 2 < m->mbw ? mx + 2 : m->mbw;
                for (int spin = 0; atomic_load(&wf->done[row - 1]) < need; spin++)
                    if (spin > 64) thread_yield();
            }
            mmc_encode_mb(m, wf->job, mx, row);
            atomic_store(&wf->done[row], mx + 1);
        }
    }
}

static void wavefront_run(void* ctx, mmc_t* m, const mmc_job_t* job) {
    wavefront_t* wf = ctx;
    if (wf->done_rows < m->mbh) {
        free(wf->done);
        wf->done = malloc(m->mbh * sizeof(atomic_uint));
        if (!wf->done) die("out of memory", NULL);
        wf->done_rows = m->mbh;
    }
    for (uint32_t i = 0; i < m->mbh; i++) atomic_init(&wf->done[i], 0);
    atomic_init(&wf->next_row, 0);
    wf->m = m;
    wf->job = job;
    int extra = wf->threads - 1;
    if (extra > (int)m->mbh - 1) extra = (int)m->mbh - 1;
    thread_t t[256];
    int started = 0;
    for (; started < extra && started < 256; started++)
        if (!thread_start(&t[started], wavefront_worker, wf)) break;
    wavefront_worker(wf);
    for (int i = 0; i < started; i++) thread_join(t[i]);
}

// Reads frames from ffmpeg on its own thread, a few ahead, so ffmpeg keeps
// decoding while frames are encoded.
#define READ_AHEAD 4
typedef struct {
    FILE* pipe;
    size_t frame_bytes;
    uint32_t* slots[READ_AHEAD];
    unsigned head, tail;        // frames taken, frames filled
    bool eof;
    mutex_t lock;
    cond_t changed;
} reader_t;

static void reader_thread(void* arg) {
    reader_t* r = arg;
    for (;;) {
        mutex_lock(&r->lock);
        while (r->tail - r->head == READ_AHEAD) cond_wait(&r->changed, &r->lock);
        uint32_t* slot = r->slots[r->tail % READ_AHEAD];
        mutex_unlock(&r->lock);
        bool ok = fread(slot, 1, r->frame_bytes, r->pipe) == r->frame_bytes;
        mutex_lock(&r->lock);
        if (ok) r->tail++;
        else r->eof = true;
        cond_broadcast(&r->changed);
        mutex_unlock(&r->lock);
        if (!ok) return;
    }
}

// The next frame, or NULL at the end. Valid until the next call.
static uint32_t* reader_next(reader_t* r, bool* taken) {
    mutex_lock(&r->lock);
    if (*taken) { r->head++; cond_broadcast(&r->changed); }
    while (r->tail == r->head && !r->eof) cond_wait(&r->changed, &r->lock);
    uint32_t* frame = r->tail != r->head ? r->slots[r->head % READ_AHEAD] : NULL;
    *taken = frame != NULL;
    mutex_unlock(&r->lock);
    return frame;
}

static const char* codec_name(uint32_t codec) {
    return codec == MVID_CODEC_H264 ? "h264" : (codec == MVID_CODEC_MOTION ? "motion" : "tiles");
}

/* H.264: x264 (through ffmpeg) encodes Constrained Baseline, the profile
 * mosmedia_h264.h decodes, with an access unit delimiter in front of
 * every frame, parameter sets at every keyframe, and full-range BT.601
 * colour like the motion codec. The stream is cut at the delimiters. */
static uint32_t encode_h264(mvid_writer_t* v, const char* in, const char* out, uint32_t w, uint32_t h, uint32_t fn,
                            uint32_t fd, uint32_t key, uint32_t q_value, bool fast, bool hwaccel, int threads) {
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error %s-i %s -an -vf scale=%u:%u:out_range=full:out_color_matrix=bt601,fps=%u/%u "
             "-c:v libx264 -profile:v baseline -preset %s -crf %u -threads %d -pix_fmt yuv420p -color_range pc "
             "-colorspace smpte170m -x264-params aud=1:repeat-headers=1:keyint=%u:fullrange=on:colormatrix=smpte170m "
             "-f h264 -",
             hwaccel ? "-hwaccel auto " : "", q(in), w, h, fn, fd, fast ? "veryfast" : "medium", q_value, threads,
             key);
    FILE* p = popen(cmd, PIPE_READ);
    if (!p) die("cannot start ffmpeg for", in);
    size_t cap = 1u << 20, have = 0, start = 0, scan = 0;
    uint8_t* buf = malloc(cap);
    if (!buf) die("out of memory", NULL);
    bool eof = false;
    while (!eof || start < have) {
        if (!eof) {
            if (cap - have < (1u << 16)) {
                /* Keep only what belongs to the frame being collected. */
                memmove(buf, buf + start, have - start);
                have -= start; scan -= start; start = 0;
                if (cap - have < (1u << 16)) {
                    cap *= 2;
                    if (!(buf = realloc(buf, cap))) die("out of memory", NULL);
                }
            }
            size_t got = fread(buf + have, 1, cap - have, p);
            have += got;
            if (!got) eof = true;
        }
        /* A frame ends where the next delimiter (NAL type 9) starts. */
        for (;;) {
            size_t end = 0;
            if (scan < start + 4) scan = start + 4;
            for (; scan + 4 <= have; scan++) {
                if (buf[scan] == 0 && buf[scan + 1] == 0 && buf[scan + 2] == 1 && (buf[scan + 3] & 31) == 9) {
                    end = scan;
                    /* The delimiter's start code may have a leading zero byte. */
                    if (end > start && buf[end - 1] == 0) end--;
                    break;
                }
            }
            if (!end && eof && have > start) end = have;
            if (!end) break;
            bool idr = false;
            for (size_t i = start; i + 4 <= end; i++)
                if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 && (buf[i + 3] & 31) == 5) idr = true;
            if (!v->info.frame_count && !idr) die("the H.264 stream does not start with a keyframe", in);
            if (!mvid_writer_h264_frame(v, buf + start, end - start, idr)) die("cannot write", out);
            if (v->info.frame_count % 30 == 0) { fprintf(stderr, "\r%u frames", v->info.frame_count); fflush(stderr); }
            start = end;
            scan = end + 4;
        }
    }
    free(buf);
    if (pclose(p) != 0) die("ffmpeg could not encode H.264 (is libx264 built in?) from", in);
    return v->info.frame_count;
}

static void cmd_vid(int argc, char** argv) {
    const char* in = argv[2];
    const char* out = argv[3];
    uint32_t want_w = 0, want_fps = 0, key = 0, loss = 0, codec = MVID_CODEC_MOTION, qp = 26;
    bool audio = true, fast = false, hwaccel = true;
    int threads = cpu_count();
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc) want_w = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) want_fps = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio")) audio = false;
        else if (!strcmp(argv[i], "--fast")) fast = true;
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc) loss = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--q") && i + 1 < argc) qp = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-hwaccel")) hwaccel = false;
        else if (!strcmp(argv[i], "--codec") && i + 1 < argc) {
            const char* c = argv[++i];
            if (!strcmp(c, "tiles")) codec = MVID_CODEC_TILES;
            else if (!strcmp(c, "motion")) codec = MVID_CODEC_MOTION;
            else if (!strcmp(c, "h264")) codec = MVID_CODEC_H264;
            else die("unknown codec (tiles, motion or h264)", c);
        }
        else die("unknown option", argv[i]);
    }
    uint32_t w, h, fn, fd;
    probe(in, &w, &h, &fn, &fd);
    if (want_w && want_w != w) {
        h = (uint32_t)((uint64_t)h * want_w / w + 1) & ~1u;
        w = want_w;
    }
    if (want_fps) { fn = want_fps; fd = 1; }
    if ((double)fn / fd > 60.0) { fn = 60; fd = 1; }
    if (!key) key = (uint32_t)((fn + fd - 1) / fd) * 2u;
    if (loss > 255) loss = 255;
    if (qp > 51) qp = 51;
    if (codec != MVID_CODEC_TILES) { w = (w + 1) & ~1u; h = (h + 1) & ~1u; }

    char cmd[8192];
    if (threads < 1) threads = 1;
    if (threads > 256) threads = 256;
    snprintf(cmd, sizeof cmd, "ffmpeg -v error %s-i %s -vf scale=%u:%u,fps=%u/%u -f rawvideo -pix_fmt bgra -",
             hwaccel ? "-hwaccel auto " : "", q(in), w, h, fn, fd);
    FILE* p = codec == MVID_CODEC_H264 ? NULL : popen(cmd, PIPE_READ);
    FILE* f = fopen(out, "w+b");
    if ((!p && codec != MVID_CODEC_H264) || !f) die("cannot start", out);
    mvid_writer_t v;
    if (!mvid_writer_begin(&v, w, h, fn, fd, true, key, fast, file_write, f, malloc, free))
        die("video too large", in);
    v.loss = loss;
    v.codec = codec;
    v.qp = qp;
    if (codec == MVID_CODEC_H264) {
        /* x264 reads and scales the source itself. */
        encode_h264(&v, in, out, w, h, fn, fd, key, qp, fast, hwaccel, threads);
    } else {
        wavefront_t wf = {.threads = threads};
        if (threads > 1) { v.parallel = wavefront_run; v.parallel_ctx = &wf; }
        reader_t rd = {.pipe = p, .frame_bytes = (size_t)w * h * 4};
        mutex_init(&rd.lock);
        cond_init(&rd.changed);
        for (int i = 0; i < READ_AHEAD; i++)
            if (!(rd.slots[i] = malloc(rd.frame_bytes))) die("out of memory", NULL);
        thread_t reader;
        if (!thread_start(&reader, reader_thread, &rd)) die("cannot start a thread", NULL);
        uint32_t* px;
        bool taken = false;
        while ((px = reader_next(&rd, &taken))) {
            if (!mvid_writer_frame(&v, px, w)) die("cannot write", out);
            if (v.info.frame_count % 30 == 0) { fprintf(stderr, "\r%u frames", v.info.frame_count); fflush(stderr); }
        }
        thread_join(reader);
        pclose(p);
        for (int i = 0; i < READ_AHEAD; i++) free(rd.slots[i]);
        free(wf.done);
    }
    if (!v.info.frame_count) die("no frames in", in);
    size_t adi_size = 0;
    uint8_t* adi = audio ? make_soundtrack(in, &adi_size) : NULL;
    uint32_t frames = v.info.frame_count;
    if (!mvid_writer_finish(&v, adi, adi_size)) die("cannot write", out);
    fseek64(f, 0, SEEK_END);
    int64_t size = ftell64(f);
    fclose(f);
    fprintf(stderr, "\r");
    printf("%s: %ux%u %s, %u frames at %.2f fps, %s, %.1f MB (%.1f%% of raw)\n", out, w, h,
           codec_name(codec), frames,
           (double)fn / fd, adi ? "with sound" : "no sound", (double)size / 1e6,
           100.0 * (double)size / ((double)w * h * 3 * frames));
    free(adi);
}

static void cmd_unvid(const char* in, const char* out) {
    FILE* f = fopen(in, "rb");
    if (!f) die("cannot open", in);
    fseek(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftell64(f);
    mvid_reader_t r;
    if (!mvid_open(&r, file_read, f, size, malloc)) die("not a .vid file", in);
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -y -f rawvideo -pix_fmt bgra -s %ux%u -framerate %u/%u -i - -pix_fmt yuv420p %s",
             r.info.width, r.info.height, r.info.fps_num, r.info.fps_den, q(out));
    size_t len = strlen(out);
    bool raw = len > 4 && !strcmp(out + len - 4, ".raw");   // the exact pixels, BGRA
    FILE* p = raw ? fopen(out, "wb") : popen(cmd, PIPE_WRITE);
    if (!p) die("cannot write", out);
    uint32_t* px = malloc((size_t)r.info.width * r.info.height * 4);
    for (uint32_t i = 0; i < r.info.frame_count; i++) {
        if (!mvid_decode(&r, i, px)) die("damaged frame in", in);
        fwrite(px, 4, (size_t)r.info.width * r.info.height, p);
    }
    if (raw ? fclose(p) != 0 : pclose(p) != 0) die("cannot finish", out);
    printf("%s: %u frames\n", out, r.info.frame_count);
    mvid_close(&r, free);
    fclose(f);
}

static void cmd_info(const char* in) {
    size_t size;
    uint8_t* d = read_file(in, &size);
    mimg_info_t img;
    mvid_info_t vid;
    if (mimg_info(d, size, &img)) {
        printf("%s: image %ux%u %s, %zu bytes\n", in, img.width, img.height, img.alpha ? "RGBA" : "RGB", size);
    } else if (size >= MVID_HEADER_SIZE && mvid_parse_header(d, size, &vid)) {
        printf("%s: video %ux%u %s, %u frames at %.2f fps (%u:%02u), keyframe every %u, %s\n", in, vid.width,
               vid.height, codec_name(vid.codec), vid.frame_count, (double)vid.fps_num / vid.fps_den, vid.duration_ms / 60000,
               vid.duration_ms / 1000 % 60, vid.keyframe_interval,
               vid.has_audio ? "with sound" : "no sound");
    } else {
        die("not an .img or .vid file", in);
    }
}

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "img")) cmd_img(argv[2], argv[3]);
    else if (argc >= 4 && !strcmp(argv[1], "png")) cmd_png(argv[2], argv[3]);
    else if (argc >= 4 && !strcmp(argv[1], "vid")) cmd_vid(argc, argv);
    else if (argc >= 4 && !strcmp(argv[1], "unvid")) cmd_unvid(argv[2], argv[3]);
    else if (argc >= 3 && !strcmp(argv[1], "info")) cmd_info(argv[2]);
    else {
        fprintf(stderr, "usage: mosmedia img|png|vid|unvid <in> <out> [options], mosmedia info <file>\n");
        return 2;
    }
    return 0;
}
