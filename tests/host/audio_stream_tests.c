// Host tests for the audio streams (src/modules/Audio/audio_streams.c):
// what programs write through SYS_AUDIO_STREAM comes out of the mixer.
// Build and run: make test-host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x86_64/audio_streams.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static uint32_t g_rate = 48000;
static int g_started;
static int g_range_ok = 1;
uint32_t audio_mix_rate(void) { return g_rate; }
void audio_ensure_running(void) { g_started++; }
static bool g_device = true;
bool audio_have_device(void) { return g_device; }
bool syscall_user_range_ok(const void* p, size_t n, bool w) { (void)p; (void)n; (void)w; return g_range_ok; }

static syscall_audio_stream_t req(uint32_t op, uint32_t handle) {
    syscall_audio_stream_t r;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.version = SYS_AUDIO_STREAM_ABI_VERSION;
    r.op = op;
    r.handle = handle;
    return r;
}

static uint32_t open_stream(uint32_t rate, uint32_t channels, uint64_t pid) {
    syscall_audio_stream_t r = req(SYS_AUDIO_STREAM_OPEN, 0);
    r.rate = rate;
    r.channels = channels;
    uint64_t h = audio_stream_syscall(&r, pid);
    return h >= 1 && h <= SYSCALL_AUDIO_STREAMS ? (uint32_t)h : 0;
}

static uint64_t write_frames(uint32_t h, const int16_t* s, uint32_t frames, uint64_t pid) {
    syscall_audio_stream_t r = req(SYS_AUDIO_STREAM_WRITE, h);
    r.samples = s;
    r.frames = frames;
    return audio_stream_syscall(&r, pid);
}

static void close_stream(uint32_t h, uint64_t pid) {
    syscall_audio_stream_t r = req(SYS_AUDIO_STREAM_CLOSE, h);
    audio_stream_syscall(&r, pid);
}

static uint32_t queued(uint32_t h, uint64_t pid) {
    syscall_audio_stream_t r = req(SYS_AUDIO_STREAM_QUERY, h);
    audio_stream_syscall(&r, pid);
    return r.queued;
}

int main(void) {
    static int16_t in[SYSCALL_AUDIO_STREAM_FRAMES * 2 + 64], out[4096 * 2];

    // Same rate, stereo: the frames come out as they went in.
    uint32_t a = open_stream(48000, 2, 7);
    CHECK(a, "a stream opens");
    for (int i = 0; i < 200; i++) { in[i * 2] = (int16_t)(i * 10); in[i * 2 + 1] = (int16_t)(-i * 10); }
    CHECK(write_frames(a, in, 200, 7) == 200 && g_started == 1, "frames are taken and the sound card is started");
    memset(out, 0, sizeof out);
    audio_streams_mix(out, 100, 48000);
    int same = 1;
    for (int i = 0; i < 100; i++) same &= out[i * 2] == i * 10 && out[i * 2 + 1] == -i * 10;
    CHECK(same, "same-rate stereo passes through unchanged");
    CHECK(queued(a, 7) == 100, "the queue shrinks by what played");
    CHECK(audio_streams_have_data(), "a stream with frames reports data");

    // It runs dry: silence after the last pair, one underrun counted.
    memset(out, 0, sizeof out);
    audio_streams_mix(out, 200, 48000);
    int tail_silent = 1;
    for (int i = 100; i < 200; i++) tail_silent &= out[i * 2] == 0;
    CHECK(tail_silent, "silence once a stream is empty");
    syscall_audio_stream_t q = req(SYS_AUDIO_STREAM_QUERY, a);
    audio_stream_syscall(&q, 7);
    CHECK(q.underruns == 1 && q.capacity == SYSCALL_AUDIO_STREAM_FRAMES && q.mix_rate == 48000, "underrun counted");
    close_stream(a, 7);
    CHECK(!audio_streams_have_data(), "closed streams report nothing");

    // Mono at 11025 Hz into a 44100 Hz mix: both channels, four times as
    // many frames, interpolated in between.
    uint32_t m = open_stream(11025, 1, 7);
    for (int i = 0; i < 101; i++) in[i] = (int16_t)(i * 300);
    write_frames(m, in, 101, 7);
    memset(out, 0, sizeof out);
    audio_streams_mix(out, 400, 44100);
    int ok = 1;
    for (int i = 0; i < 400; i++) {
        int want = i * 75;      // 300 per source frame, a quarter of a step per output frame
        ok &= out[i * 2] == out[i * 2 + 1] && abs(out[i * 2] - want) <= 1;
    }
    CHECK(ok, "mono is resampled 4x with interpolation, on both channels");
    close_stream(m, 7);

    // A full buffer takes what fits.
    uint32_t f = open_stream(22050, 2, 7);
    memset(in, 0, sizeof in);
    CHECK(write_frames(f, in, SYSCALL_AUDIO_STREAM_FRAMES - 10, 7) == SYSCALL_AUDIO_STREAM_FRAMES - 10, "big write");
    CHECK(write_frames(f, in, 64, 7) == 10, "only what fits is taken");
    CHECK(write_frames(f, in, 64, 7) == 0, "a full stream takes nothing");
    close_stream(f, 7);

    // Two streams add up and clip; volume scales.
    uint32_t x = open_stream(48000, 2, 7), y = open_stream(48000, 2, 8);
    for (int i = 0; i < 64; i++) { in[i * 2] = 30000; in[i * 2 + 1] = -1000; }
    write_frames(x, in, 64, 7);
    write_frames(y, in, 64, 8);
    syscall_audio_stream_t v = req(SYS_AUDIO_STREAM_VOLUME, y);
    v.volume = 50;
    CHECK(audio_stream_syscall(&v, 8) == SYS_SUCCESS, "volume set");
    memset(out, 0, sizeof out);
    audio_streams_mix(out, 32, 48000);
    CHECK(out[0] == 32767 && out[1] == -1500, "streams add, clip, and follow their volume");

    // Mixed on top of what is already there (the file player).
    for (int i = 0; i < 32 * 2; i++) out[i] = 100;
    audio_streams_mix(out, 4, 48000);
    CHECK(out[1] == 100 - 1000 - 500, "added to the player's output");

    // Ownership and cleanup.
    CHECK(write_frames(x, in, 4, 8) == SYS_ERR_NOTFOUND, "another process cannot write to a stream");
    audio_streams_process_exit(7);
    CHECK(write_frames(x, in, 4, 7) == SYS_ERR_NOTFOUND, "a process's streams close when it exits");
    CHECK(write_frames(y, in, 4, 8) == 4, "other processes' streams stay");
    g_range_ok = 0;
    CHECK(write_frames(y, in, 4, 8) == SYS_ERR_INVAL, "samples outside the process are refused");
    g_range_ok = 1;
    audio_streams_process_exit(8);

    // Limits.
    uint32_t hs[SYSCALL_AUDIO_STREAMS];
    for (uint32_t i = 0; i < SYSCALL_AUDIO_STREAMS; i++) hs[i] = open_stream(44100, 2, 9);
    CHECK(hs[SYSCALL_AUDIO_STREAMS - 1] != 0, "eight streams at once");
    syscall_audio_stream_t r = req(SYS_AUDIO_STREAM_OPEN, 0);
    r.rate = 44100; r.channels = 2;
    CHECK(audio_stream_syscall(&r, 9) == SYS_ERR_BUSY, "a ninth is refused");
    r.rate = 4000;
    CHECK(audio_stream_syscall(&r, 9) == SYS_ERR_INVAL, "a rate out of range is refused");
    r = req(SYS_AUDIO_STREAM_OPEN, 0);
    r.version = 99;
    CHECK(audio_stream_syscall(&r, 9) == SYS_ERR_INVAL, "a wrong ABI version is refused");
    audio_streams_process_exit(9);
    g_device = false;
    CHECK(open_stream(44100, 2, 10) == 0, "no stream without a sound card");
    g_device = true;

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("audio stream tests passed\n");
    return 0;
}
