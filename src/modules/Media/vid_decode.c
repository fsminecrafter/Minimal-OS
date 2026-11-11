#include "x86_64/media_abi.h"
#include "x86_64/allocator.h"
#include "x86_64/smp_par.h"
#include "x86_64/syscall.h"
#include "mosmedia.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern bool syscall_user_range_ok(const void* ptr, size_t len, bool write);
// fgpu.c: one H.264 picture on the card's video decoder, and the report
// that its pictures were wrong.
extern bool fgpu_video_h264(const syscall_media_h264_t* pic, uint32_t* out, uint32_t width, uint32_t height);
extern void fgpu_video_h264_mismatch(void);
// fgpu.c: the GPU backend's colour conversion; false = none, or not now.
extern bool fgpu_video_convert(const uint8_t* y, const uint8_t* cb, const uint8_t* cr, uint32_t w, uint32_t h,
                               uint32_t cw, uint32_t ch, uint32_t* out, uint32_t width, uint32_t height);

typedef struct {
    const uint8_t* data;
    size_t size;
    uint32_t tile_x;
    uint32_t tile_y;
    bool has_previous;
    bool ok;
} tile_job_t;

typedef struct {
    tile_job_t* jobs;
    uint32_t count;
    uint32_t* pixels;
    uint32_t width;
    uint32_t height;
} tile_batch_t;

static void decode_tile_job(void* context, uint32_t part, uint32_t worker) {
    (void)worker;
    tile_batch_t* batch = (tile_batch_t*)context;
    tile_job_t* job = &batch->jobs[part];
    job->ok = mm_tile_decode(job->data, job->size, batch->pixels, batch->width,
                             batch->height, batch->width, job->tile_x,
                             job->tile_y, job->has_previous);
}

// The motion codec's colour conversion on the graphics card, when its
// backend can do it (fgpu_backend_nv.c); SYS_ERR_AGAIN sends the caller
// to its own CPU conversion.
static uint64_t convert_ycbcr(syscall_media_request_t* request) {
    syscall_media_planes_t planes;
    if (request->flags || request->frame_bytes != sizeof(planes) || !request->frame ||
        !syscall_user_range_ok(request->frame, sizeof(planes), false))
        return SYS_ERR_INVAL;
    memcpy(&planes, request->frame, sizeof(planes));
    uint32_t width = request->width, height = request->height;
    if (!width || !height || width > SYS_MEDIA_MAX_SIDE || height > SYS_MEDIA_MAX_SIDE ||
        planes.w < width || planes.h < height || planes.w > SYS_MEDIA_MAX_SIDE + 16u ||
        planes.h > SYS_MEDIA_MAX_SIDE + 16u || planes.cw < (width + 1u) / 2u || planes.ch < (height + 1u) / 2u ||
        planes.cw > planes.w || planes.ch > planes.h)
        return SYS_ERR_INVAL;
    size_t luma = (size_t)planes.w * planes.h, chroma = (size_t)planes.cw * planes.ch;
    size_t pixel_bytes = (size_t)width * height * sizeof(uint32_t);
    if (!planes.y || !planes.cb || !planes.cr || !request->pixels ||
        ((uintptr_t)request->pixels & (sizeof(uint32_t) - 1u)) != 0 ||
        !syscall_user_range_ok(planes.y, luma, false) || !syscall_user_range_ok(planes.cb, chroma, false) ||
        !syscall_user_range_ok(planes.cr, chroma, false) ||
        !syscall_user_range_ok(request->pixels, pixel_bytes, true))
        return SYS_ERR_INVAL;
    return fgpu_video_convert(planes.y, planes.cb, planes.cr, planes.w, planes.h, planes.cw, planes.ch,
                              request->pixels, width, height) ? SYS_SUCCESS : SYS_ERR_AGAIN;
}

// One H.264 picture on the graphics card's video decoder. The caller's
// decoder parsed the stream; everything is checked again here.
static uint64_t decode_h264(syscall_media_request_t* request) {
    static syscall_media_h264_t pic;        // large: not on the kernel stack
    static volatile int busy;
    if (request->flags || request->frame_bytes != sizeof(pic) || !request->frame ||
        !syscall_user_range_ok(request->frame, sizeof(pic), false))
        return SYS_ERR_INVAL;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return SYS_ERR_AGAIN;
    memcpy(&pic, request->frame, sizeof(pic));
    uint32_t width = request->width, height = request->height;
    uint64_t result = SYS_ERR_INVAL;
    bool ok = pic.mb_w && pic.mb_h && pic.mb_w <= 256u && pic.mb_h <= 256u && width && height &&
              width <= (uint32_t)pic.mb_w * 16u && height <= (uint32_t)pic.mb_h * 16u && pic.max_refs <= 16u &&
              pic.cur_index < SYS_MEDIA_H264_SURFACES && pic.ref_count <= 16u && pic.slice_count &&
              pic.slice_count <= SYS_MEDIA_H264_MAX_SLICES && pic.data && pic.data_bytes &&
              pic.data_bytes <= SYS_MEDIA_MAX_FRAME_BYTES / 2u && pic.poc_type <= 2u &&
              pic.log2_max_frame_num_minus4 <= 12u && pic.log2_max_poc_lsb_minus4 <= 12u &&
              syscall_user_range_ok(pic.data, pic.data_bytes, false) && request->pixels &&
              ((uintptr_t)request->pixels & (sizeof(uint32_t) - 1u)) == 0 &&
              syscall_user_range_ok(request->pixels, (size_t)width * height * sizeof(uint32_t), true);
    for (uint32_t i = 0; ok && i < pic.ref_count; i++)
        ok = pic.refs[i].index < SYS_MEDIA_H264_SURFACES && pic.refs[i].index != pic.cur_index;
    for (uint32_t i = 0; ok && i < pic.slice_count; i++) ok = pic.slice_offset[i] < pic.data_bytes;
    if (ok) result = fgpu_video_h264(&pic, request->pixels, width, height) ? SYS_SUCCESS : SYS_ERR_AGAIN;
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return result;
}

uint64_t media_syscall(syscall_media_request_t* request) {
    if (request && syscall_user_range_ok(request, sizeof(*request), true) &&
        request->size == sizeof(*request) && request->version == SYS_MEDIA_ABI_VERSION &&
        !request->reserved[0] && !request->reserved[1]) {
        if (request->op == SYS_MEDIA_CONVERT_YCBCR) return convert_ycbcr(request);
        if (request->op == SYS_MEDIA_DECODE_H264) return decode_h264(request);
        if (request->op == SYS_MEDIA_H264_MISMATCH) { fgpu_video_h264_mismatch(); return SYS_SUCCESS; }
    }
    if (!request || !syscall_user_range_ok(request, sizeof(*request), true) ||
        request->size != sizeof(*request) ||
        request->version != SYS_MEDIA_ABI_VERSION ||
        request->op != SYS_MEDIA_DECODE_TILES ||
        (request->flags & ~SYS_MEDIA_HAS_PREVIOUS_FRAME) ||
        request->reserved[0] || request->reserved[1])
        return SYS_ERR_INVAL;

    uint32_t width = request->width, height = request->height;
    uint64_t pixel_bytes = (uint64_t)width * height * sizeof(uint32_t);
    if (!width || !height || width > SYS_MEDIA_MAX_SIDE ||
        height > SYS_MEDIA_MAX_SIDE || pixel_bytes > SYS_MEDIA_MAX_FRAME_BYTES)
        return SYS_ERR_AGAIN;
    if (request->frame_bytes > SYS_MEDIA_MAX_FRAME_BYTES)
        return SYS_ERR_AGAIN;
    if (!request->frame || !request->pixels ||
        ((uintptr_t)request->pixels & (sizeof(uint32_t) - 1u)) != 0 ||
        request->frame_bytes < 4u ||
        !syscall_user_range_ok(request->frame, (size_t)request->frame_bytes, false) ||
        !syscall_user_range_ok(request->pixels, (size_t)pixel_bytes, true))
        return SYS_ERR_INVAL;

    uint32_t tile_count = mm_tiles(width, height);
    uint32_t across = mm_tiles_x(width);
    uint32_t bitmap_bytes = (tile_count + 7u) / 8u;
    if (request->frame_bytes < 4u + (uint64_t)bitmap_bytes)
        return SYS_ERR_INVAL;

    const uint8_t* frame = request->frame;
    const uint8_t* bitmap = frame + 4u;
    uint32_t coded = 0;
    for (uint32_t tile = 0; tile < tile_count; tile++)
        coded += (bitmap[tile / 8u] >> (tile % 8u)) & 1u;

    bool has_previous = (request->flags & SYS_MEDIA_HAS_PREVIOUS_FRAME) != 0;
    if (!has_previous && coded != tile_count) return SYS_ERR_INVAL;
    if (!coded) return SYS_SUCCESS;
    uint64_t header_bytes = 4u + (uint64_t)bitmap_bytes + (uint64_t)coded * 4u;
    if (header_bytes > request->frame_bytes) return SYS_ERR_INVAL;

    tile_job_t* jobs = (tile_job_t*)alloc((size_t)coded * sizeof(*jobs));
    uint32_t* decoded = (uint32_t*)alloc((size_t)pixel_bytes);
    if (!jobs || !decoded) {
        if (jobs) free_mem(jobs);
        if (decoded) free_mem(decoded);
        return SYS_ERR_AGAIN;
    }

    const uint8_t* sizes = bitmap + bitmap_bytes;
    const uint8_t* data = frame + (size_t)header_bytes;
    size_t data_size = (size_t)(request->frame_bytes - header_bytes);
    size_t offset = 0;
    uint32_t index = 0;
    bool valid = true;
    for (uint32_t tile = 0; tile < tile_count; tile++) {
        if (!((bitmap[tile / 8u] >> (tile % 8u)) & 1u)) continue;
        uint32_t tile_size = mm_get32(sizes + (size_t)index * 4u);
        if (tile_size > data_size - offset) {
            valid = false;
            break;
        }
        jobs[index] = (tile_job_t){
            .data = data + offset,
            .size = tile_size,
            .tile_x = tile % across,
            .tile_y = tile / across,
            .has_previous = has_previous,
            .ok = false
        };
        offset += tile_size;
        index++;
    }

    if (!valid) {
        free_mem(decoded);
        free_mem(jobs);
        return SYS_ERR_INVAL;
    }

    memcpy(decoded, request->pixels, (size_t)pixel_bytes);
    tile_batch_t batch = {
        .jobs = jobs,
        .count = coded,
        .pixels = decoded,
        .width = width,
        .height = height
    };
#if defined(__STDC_HOSTED__) && !__STDC_HOSTED__
    __asm__ volatile("sti");
#endif
    smp_par_run(decode_tile_job, &batch, batch.count);

    for (uint32_t i = 0; i < coded; i++) {
        if (!jobs[i].ok) {
            free_mem(decoded);
            free_mem(jobs);
            return SYS_ERR_INVAL;
        }
    }

    memcpy(request->pixels, decoded, (size_t)pixel_bytes);
    free_mem(decoded);
    free_mem(jobs);
    return SYS_SUCCESS;
}
