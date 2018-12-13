/*
 * Videotoolbox decoder
 *
 * copyright (c) 2018 Paweł Wegner
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#ifdef CONFIG_VIDEOTOOLBOX

#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/internal.h"
#include "libavcodec/decode.h"
#include "libavcodec/h264_parse.h"
#include "videotoolbox.h"
#include <TargetConditionals.h>
#include <VideoToolbox/VideoToolbox.h>

#define BUFFER_SIZE 32

typedef struct VTContext {
    CMVideoFormatDescriptionRef video_format;
    VTDecompressionSessionRef session;
    AVFrame* frame_buffer[BUFFER_SIZE];
    int frame_buffer_size;
} VTContext;

#define AV_W8(p, v) *(p) = (v)

static int parent(int index) { return (index - 1) / 2; }

static int left(int index) { return (index + 1) * 2 - 1; }

static int right(int index) { return (index + 1) * 2; }

static void swap(AVFrame** p1, AVFrame** p2) {
    AVFrame* tmp = *p1;
    *p1 = *p2;
    *p2 = tmp;
}

static void insert_frame(VTContext* c, AVFrame* frame) {
    int node = c->frame_buffer_size;
    c->frame_buffer[c->frame_buffer_size++] = frame;
    while (node > 0) {
        if (c->frame_buffer[node]->pts < c->frame_buffer[parent(node)]->pts) {
            swap(c->frame_buffer + node, c->frame_buffer + parent(node));
            node = parent(node);
        } else {
            break;
        }
    }
}

static AVFrame* take_frame(VTContext* c) {
    AVFrame* result = c->frame_buffer[0];
    swap(c->frame_buffer, c->frame_buffer + c->frame_buffer_size - 1);
    c->frame_buffer[--c->frame_buffer_size] = NULL;
    int node = 0;
    int size = c->frame_buffer_size;
    AVFrame** buffer = c->frame_buffer;
    while (node < size) {
        int l = left(node);
        int r = right(node);
        if (l < size && buffer[l]->pts < buffer[node]->pts && (r >= size || buffer[l]->pts < buffer[r]->pts)) {
            swap(buffer + l, buffer + node);
            node = l;
        } else if (r < size && buffer[r]->pts < buffer[node]->pts) {
            swap(buffer + r, buffer + node);
            node = r;
        } else {
            break;
        }
    }
    return result;
}

static CMSampleBufferRef videotoolbox_sample_buffer_create(CMFormatDescriptionRef fmt_desc,
                                                           AVCodecContext* avctx,
                                                           AVPacket* packet) {
    OSStatus status;
    CMBlockBufferRef  block_buf = NULL;
    CMSampleBufferRef sample_buf = NULL;
    CMSampleTimingInfo timing_info = {};

    status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, // structureAllocator
                                                packet->data,        // memoryBlock
                                                packet->size,        // blockLength
                                                kCFAllocatorNull,    // blockAllocator
                                                NULL,                // customBlockSource
                                                0,                   // offsetToData
                                                packet->size,        // dataLength
                                                0,                   // flags
                                                &block_buf);

    if (!status) {
        if (packet->pts != AV_NOPTS_VALUE) {
            timing_info.presentationTimeStamp.flags = kCMTimeFlags_Valid;
            timing_info.presentationTimeStamp.value = packet->pts * avctx->time_base.num;
            timing_info.presentationTimeStamp.timescale = avctx->time_base.den;
        }
        if (packet->dts != AV_NOPTS_VALUE) {
            timing_info.decodeTimeStamp.flags = kCMTimeFlags_Valid;
            timing_info.decodeTimeStamp.value = packet->dts * avctx->time_base.num;
            timing_info.decodeTimeStamp.timescale = avctx->time_base.den;
        }
        if (packet->duration != 0) {
            timing_info.duration.flags = kCMTimeFlags_Valid;
            timing_info.duration.value = packet->duration * avctx->time_base.num;
            timing_info.duration.timescale = avctx->time_base.den;
        }
        status = CMSampleBufferCreate(kCFAllocatorDefault,  // allocator
                                      block_buf,            // dataBuffer
                                      TRUE,                 // dataReady
                                      0,                    // makeDataReadyCallback
                                      0,                    // makeDataReadyRefcon
                                      fmt_desc,             // formatDescription
                                      1,                    // numSamples
                                      1,                    // numSampleTimingEntries
                                      &timing_info,         // sampleTimingArray
                                      0,                    // numSampleSizeEntries
                                      NULL,                 // sampleSizeArray
                                      &sample_buf);
    }

    if (block_buf)
        CFRelease(block_buf);

    return sample_buf;
}

static CFDataRef videotoolbox_h264_extradata(AVCodecContext *avctx) {
    H264ParamSets ps = {};
    int is_avc;
    int nal_length_size;
    int read = ff_h264_decode_extradata(
        avctx->extradata, avctx->extradata_size, &ps, &is_avc, &nal_length_size, 0, avctx
    );
    CFDataRef result = NULL;

    if (read < 0) {
        av_log(avctx, AV_LOG_ERROR, "couldn't decode h264 extradata: %s\n", av_err2str(read));
        goto error;
    }

    ps.pps = (const PPS*)ps.pps_list[0]->data;
    ps.sps = (const SPS*)ps.sps_list[0]->data;

    int vt_extradata_size = 6 + 2 + ps.sps->data_size + 3 + ps.pps->data_size;
    uint8_t *vt_extradata = av_malloc(vt_extradata_size);
    if (!vt_extradata)
        goto error;

    uint8_t* p = vt_extradata;

    AV_W8(p + 0, 1); /* version */
    AV_W8(p + 1, ps.sps->data[1]); /* profile */
    AV_W8(p + 2, ps.sps->data[2]); /* profile compat */
    AV_W8(p + 3, ps.sps->data[3]); /* level */
    AV_W8(p + 4, 0xff); /* 6 bits reserved (111111) + 2 bits nal size length - 3 (11) */
    AV_W8(p + 5, 0xe1); /* 3 bits reserved (111) + 5 bits number of sps (00001) */
    AV_WB16(p + 6, ps.sps->data_size);
    memcpy(p + 8, ps.sps->data, ps.sps->data_size);
    p += 8 + ps.sps->data_size;
    AV_W8(p + 0, 1); /* number of pps */
    AV_WB16(p + 1, ps.pps->data_size);
    memcpy(p + 3, ps.pps->data, ps.pps->data_size);

    p += 3 + ps.pps->data_size;
    av_assert0(p - vt_extradata == vt_extradata_size);

    result = CFDataCreate(kCFAllocatorDefault, vt_extradata, vt_extradata_size);
    av_free(vt_extradata);

error:
    ff_h264_ps_uninit(&ps);
    return result;
}

static CFDictionaryRef videotoolbox_decoder_config_create(CMVideoCodecType codec_type,
                                                          AVCodecContext *avctx) {
    CFMutableDictionaryRef result = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                              0,
                                                              &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef avc_info = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                                0,
                                                                &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
    CFDataRef data = videotoolbox_h264_extradata(avctx);
    CFDictionarySetValue(avc_info, CFSTR("avcC"), data);
    CFDictionarySetValue(result, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, avc_info);
    if (data)
        CFRelease(data);
    if (avc_info)
        CFRelease(avc_info);
    return result;
}

static void videotoolbox_decoder_callback(void *opaque,
                                          void *source_frame_ref_con,
                                          OSStatus status,
                                          VTDecodeInfoFlags flags,
                                          CVImageBufferRef image_buffer,
                                          CMTime pts,
                                          CMTime duration) {
    if (status != 0) {
        av_log(opaque, AV_LOG_ERROR, "videotoolbox decoder error %d\n", status);
        return;
    }
    AVCodecContext* avctx = opaque;
    VTContext* context = avctx->priv_data;
    CVPixelBufferLockBaseAddress(image_buffer, 0);
    int plane_count = CVPixelBufferGetPlaneCount(image_buffer);
    AVFrame* frame = av_frame_alloc();
    frame->width = CVPixelBufferGetWidth(image_buffer);
    frame->height = CVPixelBufferGetHeight(image_buffer);
    for (int i = 0; i < plane_count; i++) {
        frame->linesize[i] = CVPixelBufferGetBytesPerRowOfPlane(image_buffer, i);
    }
    frame->format = AV_PIX_FMT_NV12;
    av_frame_get_buffer(frame, 0);
    if (pts.flags & kCMTimeFlags_Valid) {
        frame->best_effort_timestamp = frame->pts = pts.value * avctx->time_base.den / avctx->time_base.num / pts.timescale;
    }
    for (int i = 0; i < plane_count; i++) {
        int size = CVPixelBufferGetBytesPerRowOfPlane(image_buffer, i) * CVPixelBufferGetHeightOfPlane(image_buffer, i);
        memcpy(
            frame->data[i],
            CVPixelBufferGetBaseAddressOfPlane(image_buffer, i),
            size
        );
    }
    insert_frame(context, frame);
    CVPixelBufferUnlockBaseAddress(image_buffer, 0);
}

static int videotoolbox_init(AVCodecContext* ctx) {
    OSStatus status;
    VTDecompressionOutputCallbackRecord decoder_cb;
    VTContext* context = ctx->priv_data;
    CFDictionaryRef decoder_config;
    int result = 0;

    if (ctx->time_base.num == 0 || ctx->time_base.den == 0) {
        ctx->time_base.num = 1;
        ctx->time_base.den = 1000;
    }

    decoder_config = videotoolbox_decoder_config_create(kCMVideoCodecType_H264, ctx);
    status = CMVideoFormatDescriptionCreate(
        NULL, kCMVideoCodecType_H264, ctx->width, ctx->height,
        decoder_config, &context->video_format
    );
    if (status != 0)
        return AVERROR_UNKNOWN;

    decoder_cb.decompressionOutputCallback = videotoolbox_decoder_callback;
    decoder_cb.decompressionOutputRefCon = ctx;
    status = VTDecompressionSessionCreate(
        NULL, context->video_format, decoder_config, NULL, &decoder_cb, &context->session
    );

    if (status != 0)
        return AVERROR_UNKNOWN;

    return result;
}

static int videotoolbox_uninit(AVCodecContext* ctx) {
    VTContext* context = ctx->priv_data;
    if (context->session) {
        VTDecompressionSessionInvalidate(context->session);
        CFRelease(context->session);
        context->session = NULL;
    }
    if (context->video_format) {
        CFRelease(context->video_format);
        context->video_format = NULL;
    }
    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (context->frame_buffer[i]) {
            av_frame_free(&context->frame_buffer[i]);
        }
    }
    return 0;
}

static int videotoolbox_take_queued_frame(AVCodecContext *avctx, AVFrame* frame) {
    VTContext* context = avctx->priv_data;
    AVFrame* selected = take_frame(context);
    av_frame_unref(frame);
    av_frame_move_ref(frame, selected);
    av_frame_free(&selected);
    return 0;
}

static int videotoolbox_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    VTContext* context = avctx->priv_data;
    CMSampleBufferRef sample_buf = NULL;
    AVPacket packet;
    int status;

    if (context->frame_buffer_size >= BUFFER_SIZE) {
        return videotoolbox_take_queued_frame(avctx, frame);
    }

    status = ff_decode_get_packet(avctx, &packet);
    if (status != 0) {
        if (status == AVERROR_EOF && context->frame_buffer_size > 0) {
            status = videotoolbox_take_queued_frame(avctx, frame);
        }
        return status;
    }

    sample_buf = videotoolbox_sample_buffer_create(context->video_format, avctx, &packet);
    if (!sample_buf) {
        status = AVERROR(ENOMEM);
        goto cleanup;
    }
    status = VTDecompressionSessionDecodeFrame(context->session, sample_buf, 0, NULL, NULL);
    if (status != 0) {
        goto cleanup;
    }
    status = VTDecompressionSessionWaitForAsynchronousFrames(context->session);
    if (status != 0)
        goto cleanup;

    status = AVERROR(EAGAIN);

cleanup:
    av_packet_unref(&packet);
    if (sample_buf)
        CFRelease(sample_buf);
    return status;
}

static void videotoolbox_flush(AVCodecContext* avctx) {
    VTContext* context = avctx->priv_data;
    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (context->frame_buffer[i]) {
            av_frame_free(&context->frame_buffer[i]);
        }
    }
    context->frame_buffer_size = 0;
}

#define VIDEOTOOLBOX_DECODER(MEDIATYPE, NAME, ID, OPTS) \
    AVCodec ff_ ## NAME ## _videotoolbox_decoder = {                           \
        .name           = #NAME "_videotoolbox",                               \
        .long_name      = NULL_IF_CONFIG_SMALL(#ID " via VideoToolbox"),       \
        .type           = AVMEDIA_TYPE_ ## MEDIATYPE,                          \
        .id             = AV_CODEC_ID_ ## ID,                                  \
        .priv_data_size = sizeof(VTContext),                                   \
        .init           = videotoolbox_init,                                   \
        .close          = videotoolbox_uninit,                                 \
        .receive_frame  = videotoolbox_receive_frame,                          \
        .flush          = videotoolbox_flush,                                  \
        .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,     \
        .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE |                       \
                          FF_CODEC_CAP_INIT_CLEANUP,                           \
    };

VIDEOTOOLBOX_DECODER(VIDEO, h264, H264, NULL)

#endif /* CONFIG_VIDEOTOOLBOX */
