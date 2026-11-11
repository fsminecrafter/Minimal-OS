#ifndef MINIMALOS_MEDIA_ABI_H
#define MINIMALOS_MEDIA_ABI_H

#include <stdint.h>

#define SYS_MEDIA 45u
#define SYS_MEDIA_ABI_VERSION 1u
#define SYS_MEDIA_DECODE_TILES 1u
// The last stage of the motion codec on the graphics card: `frame` points
// to a syscall_media_planes_t (frame_bytes = its size), width x height
// pixels come back in `pixels`, exactly as mmc_to_argb makes them.
// SYS_ERR_AGAIN: no GPU can do it right now; convert on the CPU.
#define SYS_MEDIA_CONVERT_YCBCR 2u

#define SYS_MEDIA_HAS_PREVIOUS_FRAME (1u << 0)
#define SYS_MEDIA_MAX_SIDE 4096u
#define SYS_MEDIA_MAX_FRAME_BYTES (64u * 1024u * 1024u)

typedef struct {
    uint32_t size;
    uint32_t version;
    uint32_t op;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint64_t frame_bytes;
    const uint8_t* frame;
    uint32_t* pixels;
    uint32_t reserved[2];
} syscall_media_request_t;

// 4:2:0 planes, one byte per sample, rows of `w` (y) and `cw` (cb, cr).
typedef struct {
    const uint8_t* y;
    const uint8_t* cb;
    const uint8_t* cr;
    uint32_t w, h;              // luma plane (coded size, >= the picture)
    uint32_t cw, ch;            // colour planes
} syscall_media_planes_t;

#endif
