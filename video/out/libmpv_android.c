/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <libavcodec/avcodec.h>
#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include "android_common.h"
#include "demux/stheader.h"
#include "libmpv.h"
#include "misc/jni.h"
#include "mpv/render_android.h"
#include "sub/draw_bmp.h"
#include "video/hwdec.h"
#include "video/mp_image.h"

struct priv {
    struct mp_hwdec_ctx hwctx;
    ANativeWindow *osd_window;
    struct osd_state *osd;
    struct mp_draw_sub_cache *osd_cache;
    struct mp_osd_res osd_res;
    int64_t osd_id;
    double osd_pts;
    struct mp_rect src, dst;
    struct mp_image *pending;
};

static void free_device_surface(AVHWDeviceContext *device)
{
    AVMediaCodecDeviceContext *hw = device->hwctx;
    JNIEnv *env = mp_jni_get_env(NULL);
    if (env && hw->surface)
        (*env)->DeleteGlobalRef(env, hw->surface);
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    const char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;
    if (strcmp(api, MPV_RENDER_API_TYPE_ANDROID_NATIVE))
        return MPV_ERROR_NOT_IMPLEMENTED;

    const mpv_android_native_init_params *init = get_mpv_render_param(
        params, MPV_RENDER_PARAM_ANDROID_NATIVE_INIT_PARAMS, NULL);
    if (!init || init->version != MPV_ANDROID_NATIVE_API_VERSION || !init->video_surface)
        return MPV_ERROR_INVALID_PARAMETER;
    if (!avcodec_find_decoder_by_name("hevc_mediacodec"))
        return MPV_ERROR_UNSUPPORTED;

    JNIEnv *env = MP_JNI_GET_ENV(ctx);
    if (!env)
        return MPV_ERROR_UNSUPPORTED;

    jclass surface_class = (*env)->FindClass(env, "android/view/Surface");
    if (MP_JNI_EXCEPTION_LOG(ctx) < 0 || !surface_class)
        return MPV_ERROR_UNSUPPORTED;
    bool is_surface = (*env)->IsInstanceOf(env, init->video_surface, surface_class) &&
        (!init->osd_surface || (*env)->IsInstanceOf(env, init->osd_surface, surface_class));
    (*env)->DeleteLocalRef(env, surface_class);
    if (!is_surface)
        return MPV_ERROR_INVALID_PARAMETER;

    ANativeWindow *video_window = ANativeWindow_fromSurface(env, init->video_surface);
    ANativeWindow *osd_window = init->osd_surface
        ? ANativeWindow_fromSurface(env, init->osd_surface) : NULL;
    bool exception = MP_JNI_EXCEPTION_LOG(ctx) < 0;

    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;
    p->osd_window = osd_window;
    if (video_window)
        ANativeWindow_release(video_window);
    if (exception || !video_window || (init->osd_surface && !osd_window) ||
        (osd_window && (osd_window == video_window ||
            (*env)->IsSameObject(env, init->video_surface, init->osd_surface))))
        return MPV_ERROR_INVALID_PARAMETER;

    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!ref)
        return MPV_ERROR_UNSUPPORTED;
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "android-native",
        .hw_imgfmt = IMGFMT_MEDIACODEC,
        .av_device_ref = ref,
    };
    AVHWDeviceContext *device = (void *)ref->data;
    AVMediaCodecDeviceContext *hw = device->hwctx;
    device->free = free_device_surface;
    hw->surface = (*env)->NewGlobalRef(env, init->video_surface);
    if (MP_JNI_EXCEPTION_LOG(ctx) < 0 || !hw->surface || av_hwdevice_ctx_init(ref) < 0)
        return MPV_ERROR_UNSUPPORTED;

    if (osd_window)
        p->osd_cache = mp_draw_sub_alloc(p, ctx->global);
    p->osd_id = -1;
    ctx->hwdec_devs = hwdec_devices_create();
    hwdec_devices_add(ctx->hwdec_devs, &p->hwctx);
    ctx->driver_caps = VO_CAP_NATIVE_MEDIACODEC;
    return 0;
}

static enum native_video select_native(struct render_backend *ctx,
    const struct mp_codec_params *codec, enum native_policy policy, int display_hdr10)
{
    if (!codec)
        return policy == NATIVE_EXPLICIT ? NATIVE_VIDEO_GENERIC : NATIVE_VIDEO_UNSUPPORTED;
    if (display_hdr10 != 1 || !mp_native_hdr10_codec(codec))
        return NATIVE_VIDEO_UNSUPPORTED;
    return NATIVE_VIDEO_HDR10;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    return imgfmt == IMGFMT_MEDIACODEC;
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    mp_image_unrefp(&p->pending);
    p->osd_id = -1;
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    p->osd = vo ? vo->osd : NULL;
    p->osd_id = -1;
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;
    p->src = *src;
    p->dst = *dst;
    p->osd_res = *osd;
    p->osd_id = -1;
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *w, int *h)
{
    const mpv_android_native_target *target = get_mpv_render_param(
        params, MPV_RENDER_PARAM_ANDROID_NATIVE_TARGET, NULL);
    if (!target || target->version != MPV_ANDROID_NATIVE_API_VERSION ||
        target->w <= 0 || target->h <= 0)
        return MPV_ERROR_INVALID_PARAMETER;
    *w = target->w;
    *h = target->h;
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct priv *p = ctx->priv;
    mpv_android_native_target *target = get_mpv_render_param(
        params, MPV_RENDER_PARAM_ANDROID_NATIVE_TARGET, NULL);
    target->video_x = target->video_y = target->video_w = target->video_h = 0;

    struct mp_image *image = frame->current;
    p->osd_pts = image ? image->pts : MP_NOPTS_VALUE;
    if (!image)
        return 0;

    if (image->imgfmt != IMGFMT_MEDIACODEC || !image->planes[3] ||
        image->params.rotate || image->params.vflip ||
        image->params.color.primaries != PL_COLOR_PRIM_BT_2020 ||
        image->params.color.transfer != PL_COLOR_TRC_PQ ||
        image->params.repr.sys != PL_COLOR_SYSTEM_BT_2020_NC ||
        p->src.x0 || p->src.y0 || p->src.x1 != image->w || p->src.y1 != image->h) {
        MP_ERR(ctx, "Android native output needs an uncropped 10-bit BT.2020 PQ frame.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    target->video_x = p->dst.x0;
    target->video_y = p->dst.y0;
    target->video_w = mp_rect_w(p->dst);
    target->video_h = mp_rect_h(p->dst);
    // The current image may be retained for OSD redraws after its codec
    // buffer has already been returned. Only submit a newly drawn frame.
    if (!frame->redraw && !frame->repeat)
        p->pending = mp_image_new_ref(image);
    return 0;
}

static int present(struct render_backend *ctx, mpv_render_param *params, bool display)
{
    struct priv *p = ctx->priv;
    mpv_android_native_target *target = get_mpv_render_param(
        params, MPV_RENDER_PARAM_ANDROID_NATIVE_TARGET, NULL);
    int err = 0;
    if (display && p->pending) {
        AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->pending->planes[3];
        err = av_mediacodec_release_buffer(buffer, 1);
        // A seek can flush an outstanding buffer before the VO sees its reset.
        if (err < 0 && err != AVERROR(ENOENT)) {
            MP_ERR(ctx, "Android MediaCodec failed to present an output buffer.\n");
            err = MPV_ERROR_UNSUPPORTED;
        } else {
            err = 0;
        }
    }
    mp_image_unrefp(&p->pending);
    if (!display) {
        target->video_x = target->video_y = target->video_w = target->video_h = 0;
        return 0;
    }
    if (err || !p->osd_window || p->osd_res.w <= 0 || p->osd_res.h <= 0)
        return err;

    if (p->osd_id < 0 && ANativeWindow_setBuffersGeometry(p->osd_window,
            p->osd_res.w, p->osd_res.h, WINDOW_FORMAT_RGBA_8888) < 0)
        return MPV_ERROR_UNSUPPORTED;
    return android_draw_osd(ctx->log, p->osd_window, p->osd_cache, p->osd,
                            &p->osd_res, p->osd_pts, &p->osd_id) ? 0 : MPV_ERROR_GENERIC;
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    reset(ctx);
    if (p->osd_window)
        ANativeWindow_release(p->osd_window);
    if (ctx->hwdec_devs) {
        hwdec_devices_remove(ctx->hwdec_devs, &p->hwctx);
        hwdec_devices_destroy(ctx->hwdec_devs);
    }
    av_buffer_unref(&p->hwctx.av_device_ref);
}

const struct render_backend_fns render_backend_android = {
    .init = init,
    .select_native = select_native,
    .check_format = check_format,
    .reconfig = reconfig,
    .reset = reset,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .present = present,
    .destroy = destroy,
};
