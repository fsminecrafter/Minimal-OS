#include "x86_64/audio_streams.h"
#include <stddef.h>

/* Audio streams: see x86_64/audio_streams.h and SYS_AUDIO_STREAM in
 * syscall.h. */

#include "x86_64/user_ptr.h"

#define CAP SYSCALL_AUDIO_STREAM_FRAMES      /* power of two */
#define FREE 0u
#define OPEN 1u

typedef struct {
    volatile uint32_t state;
    uint64_t owner;
    uint32_t rate;
    uint32_t channels;
    volatile uint32_t volume;
    volatile uint32_t head;         /* frames written (free-running); writer only */
    volatile uint32_t tail;         /* frames played; mixer only */
    uint32_t frac;                  /* position between tail and tail + 1, 16.16 */
    volatile uint32_t underruns;
    bool running;                   /* had frames since it last ran dry */
    int16_t ring[CAP * 2];          /* stereo */
} stream_t;

static stream_t g_streams[SYSCALL_AUDIO_STREAMS];
static volatile int g_open_lock;

static void lock(void) { while (__atomic_exchange_n(&g_open_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause"); }
static void unlock(void) { __atomic_store_n(&g_open_lock, 0, __ATOMIC_RELEASE); }

static uint32_t queued(const stream_t* s) {
    return __atomic_load_n(&s->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE);
}

static int16_t clip(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

void audio_streams_mix(int16_t* out, uint32_t frames, uint32_t mix_rate) {
    if (!out || !frames || !mix_rate) return;
    for (uint32_t k = 0; k < SYSCALL_AUDIO_STREAMS; k++) {
        stream_t* s = &g_streams[k];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != OPEN) continue;
        uint32_t head = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
        uint32_t tail = s->tail;
        uint32_t step = (uint32_t)(((uint64_t)s->rate << 16) / mix_rate);
        int32_t gain = (int32_t)(s->volume > 100u ? 100u : s->volume);
        uint32_t frac = s->frac;
        for (uint32_t i = 0; i < frames; i++) {
            /* Interpolating needs this frame and the next. */
            if (head - tail < 2u) {
                if (s->running) { s->underruns++; s->running = false; }
                break;
            }
            s->running = true;
            const int16_t* a = &s->ring[(tail & (CAP - 1)) * 2];
            const int16_t* b = &s->ring[((tail + 1) & (CAP - 1)) * 2];
            for (int c = 0; c < 2; c++) {
                int32_t v = a[c] + (int32_t)(((int64_t)(b[c] - a[c]) * frac) >> 16);
                out[i * 2 + c] = clip(out[i * 2 + c] + v * gain / 100);
            }
            frac += step;
            tail += frac >> 16;
            frac &= 0xFFFFu;
        }
        s->frac = frac;
        __atomic_store_n(&s->tail, tail, __ATOMIC_RELEASE);
    }
}

bool audio_streams_have_data(void) {
    for (uint32_t k = 0; k < SYSCALL_AUDIO_STREAMS; k++)
        if (__atomic_load_n(&g_streams[k].state, __ATOMIC_ACQUIRE) == OPEN && queued(&g_streams[k]) >= 2u) return true;
    return false;
}

static void close_stream(stream_t* s) {
    __atomic_store_n(&s->state, FREE, __ATOMIC_RELEASE);
}

void audio_streams_process_exit(uint64_t pid) {
    lock();
    for (uint32_t k = 0; k < SYSCALL_AUDIO_STREAMS; k++)
        if (g_streams[k].state == OPEN && g_streams[k].owner == pid) close_stream(&g_streams[k]);
    unlock();
}

static void fill_status(const stream_t* s, syscall_audio_stream_t* r) {
    r->queued = queued(s);
    r->capacity = CAP;
    r->mix_rate = audio_mix_rate();
    r->underruns = s->underruns;
}

uint64_t audio_stream_syscall(syscall_audio_stream_t* r, uint64_t pid) {
    if (!r || r->size != sizeof(*r) || r->version != SYS_AUDIO_STREAM_ABI_VERSION) return SYS_ERR_INVAL;
    if (r->op == SYS_AUDIO_STREAM_OPEN) {
        if (r->rate < 8000u || r->rate > 96000u || r->channels < 1u || r->channels > 2u) return SYS_ERR_INVAL;
        if (!audio_have_device()) return SYS_ERR_GENERIC;     // nothing would ever play it
        lock();
        for (uint32_t k = 0; k < SYSCALL_AUDIO_STREAMS; k++) {
            stream_t* s = &g_streams[k];
            if (s->state != FREE) continue;
            s->owner = pid;
            s->rate = r->rate;
            s->channels = r->channels;
            s->volume = 100;
            s->head = s->tail = 0;
            s->frac = 0;
            s->underruns = 0;
            s->running = false;
            __atomic_store_n(&s->state, OPEN, __ATOMIC_RELEASE);
            unlock();
            r->handle = k + 1u;
            fill_status(s, r);
            return k + 1u;
        }
        unlock();
        return SYS_ERR_BUSY;
    }

    if (r->handle < 1u || r->handle > SYSCALL_AUDIO_STREAMS) return SYS_ERR_INVAL;
    stream_t* s = &g_streams[r->handle - 1u];
    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != OPEN || s->owner != pid) return SYS_ERR_NOTFOUND;

    switch (r->op) {
        case SYS_AUDIO_STREAM_WRITE: {
            if (!r->frames) { fill_status(s, r); return 0; }
            if (!r->samples || r->frames > CAP ||
                !syscall_user_range_ok(r->samples, (size_t)r->frames * s->channels * sizeof(int16_t), false))
                return SYS_ERR_INVAL;
            uint32_t head = s->head;
            uint32_t room = CAP - (head - __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE));
            uint32_t n = r->frames < room ? r->frames : room;
            const int16_t* in = r->samples;
            for (uint32_t i = 0; i < n; i++) {
                int16_t* f = &s->ring[((head + i) & (CAP - 1)) * 2];
                if (s->channels == 1) { f[0] = f[1] = in[i]; }
                else { f[0] = in[i * 2]; f[1] = in[i * 2 + 1]; }
            }
            __atomic_store_n(&s->head, head + n, __ATOMIC_RELEASE);
            if (n) audio_ensure_running();
            fill_status(s, r);
            return n;
        }
        case SYS_AUDIO_STREAM_QUERY:
            fill_status(s, r);
            return SYS_SUCCESS;
        case SYS_AUDIO_STREAM_VOLUME:
            if (r->volume > 100u) return SYS_ERR_INVAL;
            s->volume = r->volume;
            fill_status(s, r);
            return SYS_SUCCESS;
        case SYS_AUDIO_STREAM_CLOSE:
            lock();
            close_stream(s);
            unlock();
            return SYS_SUCCESS;
        default:
            return SYS_ERR_INVAL;
    }
}
