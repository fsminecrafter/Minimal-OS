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
 *         --fast       quicker, larger (delta frames only use the last frame)
 *   mosmedia unvid <in.vid> <out.mp4>         .vid -> video file (to check one;
 *                                             out.raw = the exact pixels, BGRA)
 *   mosmedia info  <file.img | file.vid>
 *
 * Build: make build/tools/mosmedia (needs ffmpeg and ffprobe; .vid sound
 * also needs python3 for tools/audioconverter/wavtoadi.py).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mosmedia.h"

static void die(const char* msg, const char* detail) {
    fprintf(stderr, "mosmedia: %s%s%s\n", msg, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

// `s` in single quotes for the shell.
static const char* q(const char* s) {
    static char buf[8][4096];
    static int slot;
    char* out = buf[slot++ & 7];
    size_t n = 0;
    out[n++] = '\'';
    for (; *s && n + 6 < sizeof(buf[0]); s++) {
        if (*s == '\'') { memcpy(out + n, "'\\''", 4); n += 4; }
        else out[n++] = *s;
    }
    out[n++] = '\'';
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
    FILE* p = popen(cmd, "r");
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
    FILE* p = popen(cmd, "r");
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
    FILE* p = popen(cmd, "w");
    if (!p || fwrite(px, 4, (size_t)info.width * info.height, p) != (size_t)info.width * info.height)
        die("ffmpeg failed", out);
    if (pclose(p) != 0) die("ffmpeg failed", out);
    printf("%s: %ux%u\n", out, info.width, info.height);
}

static bool file_write(void* ctx, uint64_t off, const void* d, size_t n) {
    FILE* f = ctx;
    return fseeko(f, (off_t)off, SEEK_SET) == 0 && fwrite(d, 1, n, f) == n;
}
static bool file_read(void* ctx, uint64_t off, void* d, size_t n) {
    FILE* f = ctx;
    return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(d, 1, n, f) == n;
}

static const char* wavtoadi_path(const char* argv0) {
    static char path[4096];
    if (access("tools/audioconverter/wavtoadi.py", R_OK) == 0) return "tools/audioconverter/wavtoadi.py";
    const char* slash = strrchr(argv0, '/');
    int len = slash ? (int)(slash - argv0) : 1;
    snprintf(path, sizeof path, "%.*s/../../tools/audioconverter/wavtoadi.py", len, slash ? argv0 : ".");
    return access(path, R_OK) == 0 ? path : NULL;
}

// The source's sound as an .adi file (IMA ADPCM, 44.1 kHz stereo); NULL if it has none.
static uint8_t* make_soundtrack(const char* in, const char* argv0, size_t* size) {
    const char* tool = wavtoadi_path(argv0);
    if (!tool) { fprintf(stderr, "mosmedia: wavtoadi.py not found, no sound\n"); return NULL; }
    char dir[] = "/tmp/mosmediaXXXXXX";
    if (!mkdtemp(dir)) return NULL;
    char wav[64], adi[64], cmd[8192];
    snprintf(wav, sizeof wav, "%s/a.wav", dir);
    snprintf(adi, sizeof adi, "%s/a.adi", dir);
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -y -i %s -vn -ac 2 -ar 44100 -c:a pcm_s16le %s", q(in), wav);
    uint8_t* data = NULL;
    if (system(cmd) == 0 && access(wav, R_OK) == 0) {
        snprintf(cmd, sizeof cmd, "python3 %s %s %s --format IADPCM > /dev/null", q(tool), wav, adi);
        if (system(cmd) == 0 && access(adi, R_OK) == 0) data = read_file(adi, size);
        else fprintf(stderr, "mosmedia: wavtoadi.py failed, no sound\n");
    }
    unlink(wav); unlink(adi); rmdir(dir);
    return data;
}

static void cmd_vid(int argc, char** argv) {
    const char* in = argv[2];
    const char* out = argv[3];
    uint32_t want_w = 0, want_fps = 0, key = 0;
    bool audio = true, fast = false;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc) want_w = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) want_fps = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio")) audio = false;
        else if (!strcmp(argv[i], "--fast")) fast = true;
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

    char cmd[8192];
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -i %s -vf scale=%u:%u,fps=%u/%u -f rawvideo -pix_fmt bgra -",
             q(in), w, h, fn, fd);
    FILE* p = popen(cmd, "r");
    FILE* f = fopen(out, "w+b");
    if (!p || !f) die("cannot start", out);
    mvid_writer_t v;
    if (!mvid_writer_begin(&v, w, h, fn, fd, true, key, fast, file_write, f, malloc, free))
        die("video too large", in);
    uint32_t* px = malloc((size_t)w * h * 4);
    while (fread(px, 4, (size_t)w * h, p) == (size_t)w * h) {
        if (!mvid_writer_frame(&v, px, w)) die("cannot write", out);
        if (v.info.frame_count % 30 == 0) { fprintf(stderr, "\r%u frames", v.info.frame_count); fflush(stderr); }
    }
    pclose(p);
    if (!v.info.frame_count) die("no frames in", in);
    size_t adi_size = 0;
    uint8_t* adi = audio ? make_soundtrack(in, argv[0], &adi_size) : NULL;
    uint32_t frames = v.info.frame_count;
    if (!mvid_writer_finish(&v, adi, adi_size)) die("cannot write", out);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    fprintf(stderr, "\r");
    printf("%s: %ux%u, %u frames at %.2f fps, %s, %.1f MB (%.1f%% of raw)\n", out, w, h, frames,
           (double)fn / fd, adi ? "with sound" : "no sound", size / 1e6,
           100.0 * (double)size / ((double)w * h * 3 * frames));
    free(adi);
}

static void cmd_unvid(const char* in, const char* out) {
    FILE* f = fopen(in, "rb");
    if (!f) die("cannot open", in);
    fseek(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(f);
    mvid_reader_t r;
    if (!mvid_open(&r, file_read, f, size, malloc)) die("not a .vid file", in);
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -y -f rawvideo -pix_fmt bgra -s %ux%u -framerate %u/%u -i - -pix_fmt yuv420p %s",
             r.info.width, r.info.height, r.info.fps_num, r.info.fps_den, q(out));
    size_t len = strlen(out);
    bool raw = len > 4 && !strcmp(out + len - 4, ".raw");   // the exact pixels, BGRA
    FILE* p = raw ? fopen(out, "wb") : popen(cmd, "w");
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
        printf("%s: video %ux%u, %u frames at %.2f fps (%u:%02u), keyframe every %u, %s\n", in, vid.width,
               vid.height, vid.frame_count, (double)vid.fps_num / vid.fps_den, vid.duration_ms / 60000,
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
