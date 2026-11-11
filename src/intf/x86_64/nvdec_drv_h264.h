/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* The H.264 part of NVIDIA's nvdec_drv.h (open-gpu-doc, classes/video):
 * the picture setup structure the NVDEC engine reads. Unchanged apart
 * from being cut down to what the H.264 path needs. */
#pragma once

typedef struct _nvdec_pass2_otf_ext_s {
    unsigned int ssm_entry_num      :16; //specifies how many SSM entries (each in unit of 8 bytes) existed in SET_SUB_SAMPLE_MAP_OFFSET surface
    unsigned int ssm_iv_num         :16; //specifies how many SSM IV (each in unit of 16 bytes) existed in SET_SUB_SAMPLE_MAP_IV_OFFSET surface
    unsigned int real_stream_length;     //the real stream length, which is the bitstream length EMD/VLD will get after whole frame SSM processing, sum up of "clear+protected" bytes in SSM entries and removing "non_slice_data/skip".
    unsigned int non_slice_data     :16; //specifies the first many bytes needed to skip, includes only those of "clear+protected" bytes ("padding" bytes excluded)
    unsigned int drm_mode           : 7;
    unsigned int reserved           : 9;
} nvdec_pass2_otf_ext_s; /* 12bytes */

typedef struct _nvdec_pass2_otf_s {
    unsigned int   wrapped_session_key[4];  // session keys
    unsigned int   wrapped_content_key[4];  // content keys
    unsigned int   initialization_vector[4];// Ctrl64 initial vector
    unsigned int   enable_encryption : 1;   // flag to enable/disable encryption
    unsigned int   key_increment     : 6;   // added to content key after unwrapping
    unsigned int   encryption_mode   : 4;
    unsigned int   key_slot_index    : 4;
    unsigned int   ssm_en            : 1;
    unsigned int   reserved1         :16;   // reserved
} nvdec_pass2_otf_s; // 0x10 bytes

typedef struct _nvdec_display_param_s
{
    unsigned int enableTFOutput    : 1; //=1, enable dbfdma to output the display surface; if disable, then the following configure on tf is useless.
    //remap for VC1
    unsigned int VC1MapYFlag       : 1;
    unsigned int MapYValue         : 3;
    unsigned int VC1MapUVFlag      : 1;
    unsigned int MapUVValue        : 3;
    //tf
    unsigned int OutStride         : 8;
    unsigned int TilingFormat      : 3;
    unsigned int OutputStructure   : 1; //(0=frame, 1=field)
    unsigned int reserved0         :11;
    int OutputTop[2];                   // in units of 256
    int OutputBottom[2];                // in units of 256
    //histogram
    unsigned int enableHistogram   : 1; // enable histogram info collection.
    unsigned int HistogramStartX   :12; // start X of Histogram window
    unsigned int HistogramStartY   :12; // start Y of Histogram window
    unsigned int reserved1         : 7;
    unsigned int HistogramEndX     :12; // end X of Histogram window
    unsigned int HistogramEndY     :12; // end y of Histogram window
    unsigned int reserved2         : 8;
} nvdec_display_param_s;  // size 0x1c bytes

// H.264
typedef struct _nvdec_dpb_entry_s  // 16 bytes
{
    unsigned int index          : 7;    // uncompressed frame buffer index
    unsigned int col_idx        : 5;    // index of associated co-located motion data buffer
    unsigned int state          : 2;    // bit1(state)=1: top field used for reference, bit1(state)=1: bottom field used for reference
    unsigned int is_long_term   : 1;    // 0=short-term, 1=long-term
    unsigned int not_existing   : 1;    // 1=marked as non-existing
    unsigned int is_field       : 1;    // set if unpaired field or complementary field pair
    unsigned int top_field_marking : 4;
    unsigned int bottom_field_marking : 4;
    unsigned int output_memory_layout : 1;  // Set according to picture level output NV12/NV24 setting.
    unsigned int reserved       : 6;
    unsigned int FieldOrderCnt[2];      // : 2*32 [top/bottom]
    int FrameIdx;                       // : 16   short-term: FrameNum (16 bits), long-term: LongTermFrameIdx (4 bits)
} nvdec_dpb_entry_s;

typedef struct _nvdec_h264_pic_s
{
    nvdec_pass2_otf_s encryption_params;
    unsigned char eos[16];
    unsigned char explicitEOSPresentFlag;
    unsigned char hint_dump_en; //enable COLOMV surface dump for all frames, which includes hints of "MV/REFIDX/QP/CBP/MBPART/MBTYPE", nvbug: 200212874
    unsigned char reserved0[2];
    unsigned int stream_len;
    unsigned int slice_count;
    unsigned int mbhist_buffer_size;     // to pass buffer size of MBHIST_BUFFER

    // Driver may or may not use based upon need.
    // If 0 then default value of 1<<27 = 298ms @ 450MHz will be used in ucode.
    // Driver can send this value based upon resolution using the formula:
    // gptimer_timeout_value = 3 * (cycles required for one frame)
    unsigned int gptimer_timeout_value;

    // Fields from msvld_h264_seq_s
    int log2_max_pic_order_cnt_lsb_minus4;
    int delta_pic_order_always_zero_flag;
    int frame_mbs_only_flag;
    int PicWidthInMbs;
    int FrameHeightInMbs;

    unsigned int tileFormat                 : 2 ;   // 0: TBL; 1: KBL; 2: Tile16x16
    unsigned int gob_height                 : 3 ;   // Set GOB height, 0: GOB_2, 1: GOB_4, 2: GOB_8, 3: GOB_16, 4: GOB_32 (NVDEC3 onwards)
    unsigned int reserverd_surface_format   : 27;

    // Fields from msvld_h264_pic_s
    int entropy_coding_mode_flag;
    int pic_order_present_flag;
    int num_ref_idx_l0_active_minus1;
    int num_ref_idx_l1_active_minus1;
    int deblocking_filter_control_present_flag;
    int redundant_pic_cnt_present_flag;
    int transform_8x8_mode_flag;

    // Fields from mspdec_h264_picture_setup_s
    unsigned int pitch_luma;                    // Luma pitch
    unsigned int pitch_chroma;                  // chroma pitch

    unsigned int luma_top_offset;               // offset of luma top field in units of 256
    unsigned int luma_bot_offset;               // offset of luma bottom field in units of 256
    unsigned int luma_frame_offset;             // offset of luma frame in units of 256
    unsigned int chroma_top_offset;             // offset of chroma top field in units of 256
    unsigned int chroma_bot_offset;             // offset of chroma bottom field in units of 256
    unsigned int chroma_frame_offset;           // offset of chroma frame in units of 256
    unsigned int HistBufferSize;                // in units of 256

    unsigned int MbaffFrameFlag           : 1;  //
    unsigned int direct_8x8_inference_flag: 1;  //
    unsigned int weighted_pred_flag       : 1;  //
    unsigned int constrained_intra_pred_flag:1; //
    unsigned int ref_pic_flag             : 1;  // reference picture (nal_ref_idc != 0)
    unsigned int field_pic_flag           : 1;  //
    unsigned int bottom_field_flag        : 1;  //
    unsigned int second_field             : 1;  // second field of complementary reference field
    unsigned int log2_max_frame_num_minus4: 4;  //  (0..12)
    unsigned int chroma_format_idc        : 2;  //
    unsigned int pic_order_cnt_type       : 2;  //  (0..2)
    int pic_init_qp_minus26               : 6;  // : 6 (-26..+25)
    int chroma_qp_index_offset            : 5;  // : 5 (-12..+12)
    int second_chroma_qp_index_offset     : 5;  // : 5 (-12..+12)

    unsigned int weighted_bipred_idc      : 2;  // : 2 (0..2)
    unsigned int CurrPicIdx               : 7;  // : 7  uncompressed frame buffer index
    unsigned int CurrColIdx               : 5;  // : 5  index of associated co-located motion data buffer
    unsigned int frame_num                : 16; //
    unsigned int frame_surfaces           : 1;  // frame surfaces flag
    unsigned int output_memory_layout     : 1;  // 0: NV12; 1:NV24. Field pair must use the same setting.

    int CurrFieldOrderCnt[2];                   // : 32 [Top_Bottom], [0]=TopFieldOrderCnt, [1]=BottomFieldOrderCnt
    nvdec_dpb_entry_s dpb[16];
    unsigned char WeightScale[6][4][4];         // : 6*4*4*8 in raster scan order (not zig-zag order)
    unsigned char WeightScale8x8[2][8][8];      // : 2*8*8*8 in raster scan order (not zig-zag order)

    // mvc setup info, must be zero if not mvc
    unsigned char num_inter_view_refs_lX[2];         // number of inter-view references
    char reserved1[14];                               // reserved for alignment
    signed char inter_view_refidx_lX[2][16];         // DPB indices (must also be marked as long-term)

    // lossless decode (At the time of writing this manual, x264 and JM encoders, differ in Intra_8x8 reference sample filtering)
    unsigned int lossless_ipred8x8_filter_enable        : 1;       // = 0, skips Intra_8x8 reference sample filtering, for vertical and horizontal predictions (x264 encoded streams); = 1, filter Intra_8x8 reference samples (JM encoded streams)
    unsigned int qpprime_y_zero_transform_bypass_flag   : 1;       // determines the transform bypass mode
    unsigned int reserved2                              : 30;      // kept for alignment; may be used for other parameters

    nvdec_display_param_s displayPara;
    nvdec_pass2_otf_ext_s ssm;

} nvdec_h264_pic_s;
