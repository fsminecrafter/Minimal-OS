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
// One H.264 picture on the graphics card's video decoder: `frame` points
// to a syscall_media_h264_t (frame_bytes = its size); width x height
// pixels of the decoded picture come back in `pixels` (full-range BT.601
// conversion, as mmc_to_argb). SYS_ERR_AGAIN: not on this machine, or not
// now; decode on the CPU.
#define SYS_MEDIA_DECODE_H264 3u
// The caller compared the card's pictures with its own decoder's and they
// differ: the kernel stops offering SYS_MEDIA_DECODE_H264 and logs it.
#define SYS_MEDIA_H264_MISMATCH 4u

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

#define SYS_MEDIA_H264_MAX_SLICES 255u
#define SYS_MEDIA_H264_SURFACES 17u         // picture buffers: 16 references and the current one
#define SYS_MEDIA_H264_RESET (1u << 0)      // flags: an IDR picture; older pictures are not needed

// A reference picture of the one being decoded.
typedef struct {
    uint8_t index;              // its picture buffer (< SYS_MEDIA_H264_SURFACES)
    uint8_t long_term;
    uint16_t reserved;
    uint32_t frame_num;         // FrameNum, or LongTermFrameIdx
    int32_t poc;                // picture order count
} syscall_media_h264_ref_t;

// Everything the card's decoder needs for one picture: the active
// sequence and picture parameter sets, the picture's own numbers, its
// reference pictures and the slices. The decoder that parsed the headers
// (mosmedia_h264.h) fills it; the kernel never parses H.264 itself.
typedef struct {
    uint16_t mb_w, mb_h;        // picture size in macroblocks
    uint8_t log2_max_frame_num_minus4, poc_type, log2_max_poc_lsb_minus4, delta_pic_order_always_zero;
    uint8_t direct_8x8_inference, max_refs, entropy_coding_mode, pic_order_present;
    uint8_t num_ref_l0_default_minus1, num_ref_l1_default_minus1, deblocking_control, redundant_pic_cnt_present;
    uint8_t transform_8x8, weighted_pred, weighted_bipred_idc, constrained_intra;
    int8_t pic_init_qp_minus26, chroma_qp_offset, second_chroma_qp_offset;
    uint8_t is_reference;       // the picture will be a reference (nal_ref_idc != 0)
    uint32_t frame_num;
    int32_t poc;
    uint8_t cur_index;          // the picture buffer this picture goes to
    uint8_t ref_count;
    uint16_t slice_count;
    uint32_t flags;             // SYS_MEDIA_H264_RESET
    syscall_media_h264_ref_t refs[16];
    // The slice NAL units, each behind a 00 00 01 start code, one after
    // the other; slice_offset[i] is where slice i's start code is.
    const uint8_t* data;
    uint32_t data_bytes;
    uint32_t slice_offset[SYS_MEDIA_H264_MAX_SLICES];
} syscall_media_h264_t;

// 4:2:0 planes, one byte per sample, rows of `w` (y) and `cw` (cb, cr).
typedef struct {
    const uint8_t* y;
    const uint8_t* cb;
    const uint8_t* cr;
    uint32_t w, h;              // luma plane (coded size, >= the picture)
    uint32_t cw, ch;            // colour planes
} syscall_media_planes_t;

#endif
