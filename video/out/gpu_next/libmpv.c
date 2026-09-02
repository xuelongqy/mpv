#include <libplacebo/config.h>

#include "common/common.h"
#include "mpv/render_gl.h"
#include "sub/osd.h"
#include "video/out/gpu_next/context.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/libmpv_gpu.h"
#include "video/out/gpu_next/video.h"
#include "video/out/libmpv.h"

struct priv {
    struct libmpv_gpu_context *context;
    struct gpu_ctx *gpu;
    struct gpu_next_renderer *renderer;
};

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    int err = libmpv_gpu_context_create(ctx, params, p, &p->context);
    if (err < 0)
        return err;

#if !defined(PL_HAVE_OPENGL)
    return MPV_ERROR_NOT_IMPLEMENTED;
#endif

    p->gpu = gpu_ctx_create_from_ra(p->context->ra_ctx, false);
    if (!p->gpu)
        return MPV_ERROR_UNSUPPORTED;

    ctx->hwdec_devs = hwdec_devices_create();
    p->renderer = gpu_next_renderer_create(p, ctx->global, ctx->log,
                                           p->gpu, ctx->hwdec_devs, true);
    if (!p->renderer)
        return MPV_ERROR_UNSUPPORTED;

    p->context->ra_ctx->opts.want_alpha =
        gpu_next_renderer_want_alpha(p->renderer);
    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_FILM_GRAIN | VO_CAP_VFLIP;
    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct priv *p = ctx->priv;
    return gpu_next_renderer_check_format(p->renderer, imgfmt);
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    struct priv *p = ctx->priv;
    if (param.type != MPV_RENDER_PARAM_ICC_PROFILE)
        return MPV_ERROR_NOT_IMPLEMENTED;
    if (!param.data)
        return MPV_ERROR_INVALID_PARAMETER;
    if (!gpu_next_renderer_use_auto_icc(p->renderer))
        return 0;

    mpv_byte_array *data = param.data;
    bstr icc = bstrdup(NULL, (bstr){data->data, data->size});
    return gpu_next_renderer_set_icc_profile(p->renderer, icc)
           ? 0 : MPV_ERROR_GENERIC;
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    struct priv *p = ctx->priv;
    gpu_next_renderer_config(p->renderer, params);
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    gpu_next_renderer_reset(p->renderer);
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    gpu_next_renderer_set_osd(p->renderer, vo ? vo->osd : NULL);
    if (!vo)
        return;

    gpu_next_renderer_update_options(p->renderer);
    p->context->ra_ctx->opts.want_alpha =
        gpu_next_renderer_want_alpha(p->renderer);
    int req_frames, max_frames;
    gpu_next_renderer_get_queue_params(p->renderer, &req_frames, &max_frames);
    vo_set_queue_params(vo, 0, req_frames, max_frames);
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;
    gpu_next_renderer_resize(p->renderer, *src, *dst, *osd);
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    mpv_opengl_fbo *fbo = get_mpv_render_param(
        params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;
    *out_w = fbo->w;
    *out_h = fbo->h;
    return 0;
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct priv *p = ctx->priv;
    struct gpu_next_target target;
    struct mp_image_params hint;
    gpu_next_renderer_prepare(p->renderer, frame, pl_color_space_monitor, 0,
                              false, &target, &hint);
    target.dither_depth = GET_MPV_RENDER_PARAM(
        params, MPV_RENDER_PARAM_DEPTH, int, 8);
    if (target.dither_depth <= 0)
        target.dither_depth = 8;

    mpv_opengl_fbo *fbo = get_mpv_render_param(
        params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo) {
        gpu_next_renderer_skip_frame(p->renderer, frame);
        return MPV_ERROR_INVALID_PARAMETER;
    }

    struct ra_tex *ra_target;
    int err = p->context->fns->wrap_fbo(p->context, params, &ra_target);
    if (err < 0) {
        gpu_next_renderer_skip_frame(p->renderer, frame);
        return err;
    }

    pl_tex target_tex = gpu_ctx_wrap_opengl_fbo(p->gpu, fbo->fbo,
                                                fbo->w, fbo->h);
    if (!target_tex) {
        gpu_next_renderer_skip_frame(p->renderer, frame);
        p->context->fns->done_frame(p->context, frame->display_synced);
        return MPV_ERROR_GENERIC;
    }

    bool alpha = p->context->ra_ctx->opts.want_alpha;
    struct pl_frame target_frame = {
        .num_planes = 1,
        .planes[0] = {
            .texture = target_tex,
            .flipped = GET_MPV_RENDER_PARAM(
                params, MPV_RENDER_PARAM_FLIP_Y, int, 0),
            .components = alpha ? 4 : 3,
            .component_mapping = {0, 1, 2, 3},
        },
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_monitor,
        .crop = {
            .x1 = fbo->w,
            .y1 = fbo->h,
        },
    };
    target_frame.repr.alpha = alpha ? PL_ALPHA_INDEPENDENT : PL_ALPHA_NONE;

    struct gpu_next_render_result result;
    bool ok = gpu_next_renderer_render(p->renderer, frame, &target_frame, &target,
                                       &result);
    pl_tex_destroy(p->gpu->gpu, &target_tex);
    p->context->fns->done_frame(p->context, frame->display_synced);
    return ok ? 0 : MPV_ERROR_GENERIC;
}

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    struct priv *p = ctx->priv;
    return gpu_next_renderer_get_image(p->renderer, imgfmt, w, h,
                                       stride_align, flags);
}

static void screenshot(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args)
{
    struct priv *p = ctx->priv;
    gpu_next_renderer_screenshot(p->renderer, frame, args);
}

static void perfdata(struct render_backend *ctx,
                     struct voctrl_performance_data *out)
{
    struct priv *p = ctx->priv;
    gpu_next_renderer_perfdata(p->renderer, out);
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    gpu_next_renderer_destroy(&p->renderer);
    hwdec_devices_destroy(ctx->hwdec_devs);
    ctx->hwdec_devs = NULL;
    gpu_ctx_destroy(&p->gpu);
    libmpv_gpu_context_destroy(&p->context);
}

const struct render_backend_fns render_backend_gpu_next = {
    .init = init,
    .check_format = check_format,
    .set_parameter = set_parameter,
    .reconfig = reconfig,
    .reset = reset,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .get_image = get_image,
    .screenshot = screenshot,
    .perfdata = perfdata,
    .destroy = destroy,
};
