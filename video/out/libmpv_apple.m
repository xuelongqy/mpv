#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dovi_meta.h>
#include <math.h>
#include "common/common.h"
#include "common/msg.h"
#include "demux/stheader.h"
#include "mpv/render_apple.h"
#include "sub/draw_bmp.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "libmpv.h"

struct priv {
    void *layer;
    void *renderer;
    CMSampleBufferRef pending;
    struct mp_hwdec_ctx hwctx;
    struct osd_state *osd;
    struct mp_draw_sub_cache *draw_cache;
    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
    CGImageRef overlay;
    int64_t overlay_id;
    bool native_dv;
};

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    const char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;
    if (strcmp(api, MPV_RENDER_API_TYPE_APPLE_NATIVE))
        return MPV_ERROR_NOT_IMPLEMENTED;

    mpv_apple_native_init_params *init = get_mpv_render_param(
        params, MPV_RENDER_PARAM_APPLE_NATIVE_INIT_PARAMS, NULL);
    if (!init || init->version != MPV_APPLE_NATIVE_API_VERSION || !init->display_layer)
        return MPV_ERROR_INVALID_PARAMETER;
    if (!__builtin_available(macOS 14.0, iOS 17.0, *))
        return MPV_ERROR_UNSUPPORTED;

    AVSampleBufferDisplayLayer *layer = (__bridge id)init->display_layer;
    if (![layer isKindOfClass:AVSampleBufferDisplayLayer.class] || layer.controlTimebase)
        return MPV_ERROR_INVALID_PARAMETER;

    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;
    p->layer = (void *)CFBridgingRetain(layer);
    p->renderer = (void *)CFBridgingRetain(layer.sampleBufferRenderer);
    p->draw_cache = mp_draw_sub_alloc(p, ctx->global);
    ctx->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "apple-native",
        .hw_imgfmt = IMGFMT_VIDEOTOOLBOX,
    };
    if (av_hwdevice_ctx_create(&p->hwctx.av_device_ref, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                              NULL, NULL, 0) < 0)
        return MPV_ERROR_UNSUPPORTED;
    hwdec_devices_add(ctx->hwdec_devs, &p->hwctx);
    AVCodecContext *codec = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));
    if (codec) {
        p->native_dv = av_opt_find(codec, "vt_native_dv", NULL, 0, AV_OPT_SEARCH_CHILDREN);
        avcodec_free_context(&codec);
    }
    ctx->driver_caps = VO_CAP_NATIVE_VIDEOTOOLBOX;
    return 0;
}

bool mpv_apple_native_supported(struct render_backend *ctx,
                                const struct mp_codec_params *codec)
{
    struct priv *p = ctx->priv;
    if (!codec || !codec->lav_codecpar || codec->rotate)
        return false;
    const AVCodecParameters *params = codec->lav_codecpar;
    if (params->codec_id != AV_CODEC_ID_H264 && params->codec_id != AV_CODEC_ID_HEVC)
        return false;
    const AVPacketSideData *sd = av_packet_side_data_get(params->coded_side_data,
        params->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    if (!codec->dovi && !sd)
        return true;
    if (!p->native_dv || !AVPlayer.eligibleForHDRPlayback || !sd ||
        sd->size < sizeof(AVDOVIDecoderConfigurationRecord))
        return false;
    const AVDOVIDecoderConfigurationRecord *dv = (const void *)sd->data;
    return dv->dv_version_major == 1 && dv->dv_profile == 5 && dv->dv_level <= 63 &&
           dv->rpu_present_flag && dv->bl_present_flag && !dv->el_present_flag &&
           !dv->dv_bl_signal_compatibility_id && dv->dv_md_compression == AV_DOVI_COMPRESSION_NONE;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    return imgfmt == IMGFMT_VIDEOTOOLBOX;
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (p->pending)
        CFRelease(p->pending);
    p->pending = NULL;
    AVSampleBufferVideoRenderer *renderer = (__bridge id)p->renderer;
    [renderer flushWithRemovalOfDisplayedImage:YES completionHandler:nil];
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    p->osd = vo ? vo->osd : NULL;
    p->overlay_id = 0;
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;
    p->src = *src;
    p->dst = *dst;
    p->osd_res = *osd;
    p->overlay_id = 0;
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *w, int *h)
{
    mpv_apple_native_target *target = get_mpv_render_param(
        params, MPV_RENDER_PARAM_APPLE_NATIVE_TARGET, NULL);
    if (!target || target->version != MPV_APPLE_NATIVE_API_VERSION ||
        target->w <= 0 || target->h <= 0 || target->osd)
        return MPV_ERROR_INVALID_PARAMETER;
    *w = target->w;
    *h = target->h;
    return 0;
}

static int update_overlay(struct render_backend *ctx, double pts)
{
    struct priv *p = ctx->priv;
    if (!p->osd) {
        if (p->overlay)
            CGImageRelease(p->overlay);
        p->overlay = NULL;
        return 0;
    }
    struct sub_bitmap_list *list = osd_render(p->osd, p->osd_res, pts, 0,
                                             mp_draw_sub_formats);
    if (p->overlay_id == list->change_id) {
        talloc_free(list);
        return 0;
    }
    if (p->overlay)
        CGImageRelease(p->overlay);
    p->overlay = NULL;
    if (list->num_items) {
        struct mp_rect active, modified;
        int num_active, num_modified;
        struct mp_image *image = mp_draw_sub_overlay(p->draw_cache, list,
            &active, 1, &num_active, &modified, 1, &num_modified);
        if (!image) {
            talloc_free(list);
            return MPV_ERROR_GENERIC;
        }
        CFDataRef data = CFDataCreate(NULL, image->planes[0], image->stride[0] * image->h);
        CGDataProviderRef provider = data ? CGDataProviderCreateWithCFData(data) : NULL;
        CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if (provider && color)
            p->overlay = CGImageCreate(image->w, image->h, 8, 32, image->stride[0],
                color, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
                provider, NULL, false, kCGRenderingIntentDefault);
        if (color)
            CGColorSpaceRelease(color);
        if (provider)
            CGDataProviderRelease(provider);
        if (data)
            CFRelease(data);
        if (!p->overlay) {
            talloc_free(list);
            return MPV_ERROR_GENERIC;
        }
    }
    p->overlay_id = list->change_id;
    talloc_free(list);
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct priv *p = ctx->priv;
    mpv_apple_native_target *target = get_mpv_render_param(
        params, MPV_RENDER_PARAM_APPLE_NATIVE_TARGET, NULL);
    target->video_rect = CGRectMake(0, 0, target->w, target->h);
    target->contents_rect = CGRectMake(0, 0, 1, 1);
    if (p->pending)
        CFRelease(p->pending);
    p->pending = NULL;
    if (!frame->current) {
        reset(ctx);
    } else {
        struct mp_image *image = frame->current;
        if (image->imgfmt != IMGFMT_VIDEOTOOLBOX || !image->planes[3] ||
            image->params.rotate || image->params.vflip)
            return MPV_ERROR_UNSUPPORTED;
        CVPixelBufferRef buffer = (CVPixelBufferRef)image->planes[3];
        CFTypeRef rpu = CVBufferCopyAttachment(buffer, CFSTR("DolbyVisionRPUData"), NULL);
        bool has_rpu = rpu && CFGetTypeID(rpu) == CFDataGetTypeID() && CFDataGetLength(rpu);
        if (rpu)
            CFRelease(rpu);
        if (image->params.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION && !has_rpu) {
            MP_ERR(ctx, "Native DV metadata is unavailable; refusing an unprocessed frame.\n");
            return MPV_ERROR_UNSUPPORTED;
        }
        CMVideoFormatDescriptionRef format = NULL;
        OSStatus status = CMVideoFormatDescriptionCreateForImageBuffer(NULL, buffer, &format);
        double pts = image->pts != MP_NOPTS_VALUE && isfinite(image->pts) ? image->pts : 0;
        CMSampleTimingInfo timing = {kCMTimeInvalid,
            CMTimeMakeWithSeconds(pts, 1000000000), kCMTimeInvalid};
        if (!status)
            status = CMSampleBufferCreateReadyWithImageBuffer(NULL, buffer, format,
                                                             &timing, &p->pending);
        if (format)
            CFRelease(format);
        if (status)
            return MPV_ERROR_UNSUPPORTED;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(p->pending, true);
        CFMutableDictionaryRef first = (void *)CFArrayGetValueAtIndex(attachments, 0);
        // mpv waits for presentation; the layer must not run a second media clock.
        CFDictionarySetValue(first, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
        target->video_rect = CGRectMake(p->dst.x0, p->dst.y0,
                                        mp_rect_w(p->dst), mp_rect_h(p->dst));
        target->contents_rect = CGRectMake((double)p->src.x0 / image->w,
            (double)p->src.y0 / image->h, (double)mp_rect_w(p->src) / image->w,
            (double)mp_rect_h(p->src) / image->h);
    }
    int err = update_overlay(ctx, frame->current ? frame->current->pts : 0);
    if (!err && p->overlay)
        target->osd = CGImageRetain(p->overlay);
    return err;
}

static int present(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p->pending)
        return 0;
    AVSampleBufferVideoRenderer *renderer = (__bridge id)p->renderer;
    if (renderer.status == AVQueuedSampleBufferRenderingStatusFailed) {
        MP_ERR(ctx, "Apple video presentation failed: %s\n", renderer.error.description.UTF8String);
        CFRelease(p->pending);
        p->pending = NULL;
        return MPV_ERROR_UNSUPPORTED;
    }
    // Immediate frames replace previous queued images instead of accumulating.
    [renderer enqueueSampleBuffer:p->pending];
    CFRelease(p->pending);
    p->pending = NULL;
    return renderer.status == AVQueuedSampleBufferRenderingStatusFailed
           ? MPV_ERROR_UNSUPPORTED : 0;
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    if (p->renderer) {
        reset(ctx);
        CFRelease(p->renderer);
    }
    if (p->layer)
        CFRelease(p->layer);
    if (p->overlay)
        CGImageRelease(p->overlay);
    if (ctx->hwdec_devs) {
        hwdec_devices_remove(ctx->hwdec_devs, &p->hwctx);
        hwdec_devices_destroy(ctx->hwdec_devs);
    }
    av_buffer_unref(&p->hwctx.av_device_ref);
}

const struct render_backend_fns render_backend_apple = {
    .init = init,
    .check_format = check_format,
    .set_parameter = set_parameter,
    .reconfig = reconfig,
    .reset = reset,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .present = present,
    .destroy = destroy,
};
