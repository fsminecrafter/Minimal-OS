#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "x86_64/nv_hw.h"
#include "x86_64/media_abi.h"
#include "x86_64/nvdec_drv_h264.h"

/*
 * H.264 on NVDEC, the video decoder of NVIDIA cards (class
 * NVC4B0_VIDEO_DECODER on Turing): the engine reads the slices itself and
 * decodes whole pictures. The driver's part is small - a picture setup
 * structure (nvdec_h264_pic_s, NVIDIA's nvdec_drv.h), the buffers the
 * engine works in, and a dozen methods per picture.
 *
 * Values and order follow Mesa's NVK (src/nouveau/vulkan/rust/video/
 * decode/h264.rs), method addresses NVIDIA's clc5b0.h. No kernel
 * dependencies (tests/host/nv_sass_tests.c checks what is built here);
 * memory and submission are in fgpu_backend_nv.c.
 *
 * Pictures are NV12 in block-linear memory: a luma plane, and a plane of
 * Cb, Cr pairs at half the size. The 3D engine samples both as textures
 * to convert a picture to RGB.
 */

#define NV_CLASS_NVDEC_TURING   0xC4B0u
#define NV_NVDEC_SUBCHANNEL     4u

// Where a picture's planes go and how big they are.
typedef struct {
    uint32_t width, height;         // luma, in pixels (multiples of 16)
    uint32_t pitch;                 // bytes per row of both planes (a multiple of 64)
    uint32_t block_log2;            // block height in GOBs (8 rows), as a power of two; >= 1
    uint32_t luma_bytes, chroma_bytes;
} nvdec_layout_t;

void nvdec_layout(uint32_t mb_w, uint32_t mb_h, nvdec_layout_t* l);

// Sizes of the engine's working buffers for a stream.
void nvdec_buffer_sizes(uint32_t mb_w, uint32_t mb_h, uint32_t max_refs, uint32_t* coloc, uint32_t* mbhist,
                        uint32_t* history);

// The picture setup structure. `stream_bytes`: the slice data, without
// the end-of-stream marker that follows it in the buffer.
void nvdec_h264_setup(nvdec_h264_pic_s* out, const syscall_media_h264_t* pic, const nvdec_layout_t* l,
                      uint32_t stream_bytes);

// What goes behind the slice data.
extern const uint8_t nvdec_h264_eos[16];

typedef struct {
    // GPU virtual addresses, all multiples of 256.
    uint64_t setup, stream, slice_offsets, coloc, history, mbhist, status;
    uint64_t luma[SYS_MEDIA_H264_SURFACES], chroma[SYS_MEDIA_H264_SURFACES];    // 0 = no such picture
    uint32_t picture_number;        // counts pictures
    uint32_t max_refs;
} nvdec_job_t;

void nvdec_push_h264(nv_push_t* p, const nvdec_job_t* job);

/* Methods (clc5b0.h; the Turing class has the same ones). */
#define NVDEC_SET_APPLICATION_ID        0x0200u
#define NVDEC_EXECUTE                   0x0300u
#define NVDEC_SET_CONTROL_PARAMS        0x0400u
#define NVDEC_SET_DRV_PIC_SETUP_OFFSET  0x0404u
#define NVDEC_SET_IN_BUF_BASE_OFFSET    0x0408u
#define NVDEC_SET_PICTURE_INDEX         0x040Cu
#define NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET 0x0410u
#define NVDEC_SET_COLOC_DATA_OFFSET     0x0414u
#define NVDEC_SET_HISTORY_OFFSET        0x0418u
#define NVDEC_SET_NVDEC_STATUS_OFFSET   0x0424u
#define NVDEC_SET_PICTURE_LUMA_OFFSET(i)   (0x0430u + (i) * 4u)
#define NVDEC_SET_PICTURE_CHROMA_OFFSET(i) (0x0474u + (i) * 4u)
#define NVDEC_H264_SET_MBHIST_BUF_OFFSET 0x0500u
#define NVDEC_CODEC_H264                3u
