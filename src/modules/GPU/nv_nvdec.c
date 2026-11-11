#include "x86_64/nv_nvdec.h"
#include <stddef.h>

/* H.264 on NVDEC: the picture setup and the methods (nv_nvdec.h). */

const uint8_t nvdec_h264_eos[16] = { 0, 0, 1, 0x0B, 0, 0, 0, 0, 0, 0, 1, 0x0B, 0, 0, 0, 0 };

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

void nvdec_layout(uint32_t mb_w, uint32_t mb_h, nvdec_layout_t* l) {
    l->width = mb_w * 16u;
    l->height = mb_h * 16u;
    l->pitch = align_up(l->width, 64u);
    // Blocks of 16 GOBs (128 rows) for ordinary sizes, smaller ones for
    // small pictures; the decoder's smallest block is 2 GOBs.
    l->block_log2 = 4;
    while (l->block_log2 > 1u && (8u << l->block_log2) / 2u >= l->height) l->block_log2--;
    uint32_t block_rows = 8u << l->block_log2;
    l->luma_bytes = align_up(l->pitch * align_up(l->height, block_rows), 4096u);
    l->chroma_bytes = align_up(l->pitch * align_up(l->height / 2u, block_rows), 4096u);
}

void nvdec_buffer_sizes(uint32_t mb_w, uint32_t mb_h, uint32_t max_refs, uint32_t* coloc, uint32_t* mbhist,
                        uint32_t* history) {
    *coloc = align_up(align_up(mb_h, 2u) * mb_w * 64u - 63u, 0x100u) * (max_refs + 1u);
    *mbhist = align_up(mb_w * 104u, 0x100u);
    *history = align_up(mb_w * 0x300u, 0x200u);
}

void nvdec_h264_setup(nvdec_h264_pic_s* out, const syscall_media_h264_t* pic, const nvdec_layout_t* l,
                      uint32_t stream_bytes) {
    __builtin_memset(out, 0, sizeof(*out));
    uint32_t coloc, mbhist, history;
    nvdec_buffer_sizes(pic->mb_w, pic->mb_h, pic->max_refs, &coloc, &mbhist, &history);
    __builtin_memcpy(out->eos, nvdec_h264_eos, sizeof(out->eos));
    out->explicitEOSPresentFlag = 1;
    out->stream_len = stream_bytes + (uint32_t)sizeof(nvdec_h264_eos);
    out->slice_count = pic->slice_count;
    out->mbhist_buffer_size = mbhist;
    out->gptimer_timeout_value = 81000000u;

    out->log2_max_pic_order_cnt_lsb_minus4 = pic->log2_max_poc_lsb_minus4;
    out->delta_pic_order_always_zero_flag = pic->delta_pic_order_always_zero;
    out->frame_mbs_only_flag = 1;
    out->PicWidthInMbs = pic->mb_w;
    out->FrameHeightInMbs = pic->mb_h;
    out->tileFormat = 1;                            // block-linear as the 3D engine reads it
    out->gob_height = l->block_log2 - 1u;

    out->entropy_coding_mode_flag = pic->entropy_coding_mode;
    out->pic_order_present_flag = pic->pic_order_present;
    out->num_ref_idx_l0_active_minus1 = pic->num_ref_l0_default_minus1;
    out->num_ref_idx_l1_active_minus1 = pic->num_ref_l1_default_minus1;
    out->deblocking_filter_control_present_flag = pic->deblocking_control;
    out->redundant_pic_cnt_present_flag = pic->redundant_pic_cnt_present;
    out->transform_8x8_mode_flag = pic->transform_8x8;

    out->pitch_luma = l->pitch;
    out->pitch_chroma = l->pitch;
    out->luma_bot_offset = (uint32_t)pic->mb_w * 16u;
    out->chroma_bot_offset = l->pitch / 2u;
    out->HistBufferSize = history >> 8;

    out->direct_8x8_inference_flag = pic->direct_8x8_inference;
    out->weighted_pred_flag = pic->weighted_pred;
    out->constrained_intra_pred_flag = pic->constrained_intra;
    out->ref_pic_flag = pic->is_reference;
    out->log2_max_frame_num_minus4 = pic->log2_max_frame_num_minus4;
    out->chroma_format_idc = 1;
    out->pic_order_cnt_type = pic->poc_type;
    out->pic_init_qp_minus26 = pic->pic_init_qp_minus26;
    out->chroma_qp_index_offset = pic->chroma_qp_offset;
    out->second_chroma_qp_index_offset = pic->second_chroma_qp_offset;
    out->weighted_bipred_idc = pic->weighted_bipred_idc;
    out->CurrPicIdx = pic->cur_index;
    out->CurrColIdx = pic->cur_index;
    out->frame_num = pic->frame_num;
    out->CurrFieldOrderCnt[0] = pic->poc;
    out->CurrFieldOrderCnt[1] = pic->poc;

    // Reference pictures: frames, short- or long-term.
    for (uint32_t i = 0; i < pic->ref_count && i < 16u; i++) {
        const syscall_media_h264_ref_t* r = &pic->refs[i];
        nvdec_dpb_entry_s* e = &out->dpb[i];
        uint32_t marking = r->long_term ? 2u : 1u;
        e->index = r->index;
        e->col_idx = r->index;
        e->state = 3;                               // both fields: a frame
        e->is_long_term = r->long_term ? 1u : 0u;
        e->top_field_marking = marking;
        e->bottom_field_marking = marking;
        e->FieldOrderCnt[0] = (uint32_t)r->poc;
        e->FieldOrderCnt[1] = (uint32_t)r->poc;
        e->FrameIdx = (int)r->frame_num;
    }
    // No scaling matrices: flat.
    __builtin_memset(out->WeightScale, 16, sizeof(out->WeightScale));
    __builtin_memset(out->WeightScale8x8, 16, sizeof(out->WeightScale8x8));
}

static void m(nv_push_t* p, uint32_t method, uint32_t value) { nv_push_1(p, NV_NVDEC_SUBCHANNEL, method, value); }

// Buffers are named by their address / 256.
static uint32_t at(uint64_t va) { return (uint32_t)(va >> 8); }

void nvdec_push_h264(nv_push_t* p, const nvdec_job_t* job) {
    m(p, 0x0000, NV_CLASS_NVDEC_TURING);            // the class on this subchannel
    m(p, NVDEC_SET_APPLICATION_ID, NVDEC_CODEC_H264);
    // H.264; watchdog timers on; errors are concealed, not returned.
    uint32_t error_frame = job->max_refs ? job->picture_number % job->max_refs : 0u;
    m(p, NVDEC_SET_CONTROL_PARAMS, NVDEC_CODEC_H264 | (1u << 4) | (1u << 6) | ((error_frame & 63u) << 7) | (1u << 13));
    m(p, NVDEC_SET_DRV_PIC_SETUP_OFFSET, at(job->setup));
    m(p, NVDEC_SET_IN_BUF_BASE_OFFSET, at(job->stream));
    m(p, NVDEC_SET_PICTURE_INDEX, job->picture_number);
    m(p, NVDEC_SET_SLICE_OFFSETS_BUF_OFFSET, at(job->slice_offsets));
    m(p, NVDEC_SET_COLOC_DATA_OFFSET, at(job->coloc));
    m(p, NVDEC_SET_HISTORY_OFFSET, at(job->history));
    m(p, NVDEC_SET_NVDEC_STATUS_OFFSET, at(job->status));
    for (uint32_t i = 0; i < SYS_MEDIA_H264_SURFACES; i++) m(p, NVDEC_SET_PICTURE_LUMA_OFFSET(i), at(job->luma[i]));
    for (uint32_t i = 0; i < SYS_MEDIA_H264_SURFACES; i++)
        m(p, NVDEC_SET_PICTURE_CHROMA_OFFSET(i), at(job->chroma[i]));
    m(p, NVDEC_H264_SET_MBHIST_BUF_OFFSET, at(job->mbhist));
    m(p, NVDEC_EXECUTE, 0);                         // no notification: a semaphore follows
}
