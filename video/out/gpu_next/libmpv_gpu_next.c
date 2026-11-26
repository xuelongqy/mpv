/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libplacebo/config.h>

#include "config.h"
#include "common/common.h"
#include "mpv/render_gl.h"
#include "sub/osd.h"
#include "video/out/gpu_next/context.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu/libmpv_gpu.h"
#include "video/out/gpu_next/video.h"
#include "video/out/libmpv.h"
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
#include "mpv/render_d3d11.h"
#include "osdep/windows_utils.h"
#include "video/out/d3d11/ra_d3d11.h"
#endif
#if HAVE_VULKAN
#include "libmpv_vk.h"
#endif

struct priv {
    struct libmpv_gpu_context *context;
    struct gpu_ctx *gpu;
    struct mppl_video *renderer;
    bstr icc_profile;
#if HAVE_VULKAN
    struct libmpv_vk *vk;
#endif
};

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    const char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;
    bool vulkan = !strcmp(api, MPV_RENDER_API_TYPE_VULKAN);
    bool opengl = !strcmp(api, MPV_RENDER_API_TYPE_OPENGL);
    bool d3d11 = !strcmp(api, MPV_RENDER_API_TYPE_D3D11_GPU_NEXT);
    if (!vulkan && !opengl && !d3d11)
        return MPV_ERROR_NOT_IMPLEMENTED;
#if !(HAVE_D3D11 && defined(PL_HAVE_D3D11))
    if (d3d11)
        return MPV_ERROR_NOT_IMPLEMENTED;
#endif

    if (vulkan) {
#if HAVE_VULKAN
        int err = libmpv_vk_create(ctx, params, &p->vk, &p->gpu);
        if (err < 0)
            return err;
#else
        return MPV_ERROR_NOT_IMPLEMENTED;
#endif
    } else {
        int err = libmpv_gpu_context_create(ctx->global, ctx->log, params,
                                            &p->context);
        if (err < 0)
            return err;

#if !defined(PL_HAVE_OPENGL)
        if (opengl)
            return MPV_ERROR_NOT_IMPLEMENTED;
#endif

        p->gpu = gpu_ctx_create(ctx->log, p->context->ra_ctx, false);
        if (!p->gpu)
            return MPV_ERROR_UNSUPPORTED;
    }

    ctx->hwdec_devs = hwdec_devices_create();
    p->renderer = mppl_video_init(ctx->log, ctx->global, p->gpu);
    mppl_video_init_hwdecs(p->renderer, ctx->hwdec_devs, !vulkan);

    p->gpu->ra_ctx->opts.want_alpha =
        mppl_video_want_alpha(p->renderer);
    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_FILM_GRAIN | VO_CAP_VFLIP;
    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct priv *p = ctx->priv;
    return mppl_video_check_format(p->renderer, imgfmt);
}

// Reloading renderer options can discard the active profile.
static bool update_icc_profile(struct priv *p)
{
    return !mppl_video_icc_auto_enabled(p->renderer) ||
           mppl_video_set_icc_profile(p->renderer, bstrdup(NULL, p->icc_profile));
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    struct priv *p = ctx->priv;
#if HAVE_VULKAN
    if (p->vk && param.type == MPV_RENDER_PARAM_VULKAN_RETIRE_TARGET)
        return libmpv_vk_retire(p->vk, param.data);
#endif
    if (param.type != MPV_RENDER_PARAM_ICC_PROFILE)
        return MPV_ERROR_NOT_IMPLEMENTED;
    if (!param.data)
        return MPV_ERROR_INVALID_PARAMETER;

    mpv_byte_array *data = param.data;
    talloc_free(p->icc_profile.start);
    p->icc_profile = bstrdup(p, (bstr){data->data, data->size});
    return update_icc_profile(p) ? 0 : MPV_ERROR_GENERIC;
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    struct priv *p = ctx->priv;
    if (!params->imgfmt)
        mppl_video_flush(p->renderer);
    mppl_video_config(p->renderer, params);
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    // Also used when the output policy switches render backends midstream.
    mppl_video_reset(p->renderer, false);
}

static void set_paused(struct render_backend *ctx, bool paused)
{
    struct priv *p = ctx->priv;
    mppl_video_set_paused(p->renderer, paused);
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    mppl_video_set_osd_source(p->renderer, vo ? vo->osd : NULL);
    if (!vo)
        return;

    mppl_video_reload_options(p->renderer);
    update_icc_profile(p);
    p->gpu->ra_ctx->opts.want_alpha = mppl_video_want_alpha(p->renderer);
    mppl_video_configure_queue(p->renderer, vo);
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;
    mppl_video_resize(p->renderer, *src, *dst, *osd);
}

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
static int get_d3d11_target_desc(struct render_backend *ctx, mpv_render_param *params,
                                D3D11_TEXTURE2D_DESC *desc)
{
    const mpv_d3d11_fbo *fbo = get_mpv_render_param(
        params, MPV_RENDER_PARAM_D3D11_FBO, NULL);
    if (!fbo || !fbo->tex)
        return MPV_ERROR_INVALID_PARAMETER;

    ID3D11Texture2D *tex = fbo->tex;
    D3D11_RESOURCE_DIMENSION type;
    ID3D11Texture2D_GetType(tex, &type);
    if (type != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return MPV_ERROR_INVALID_PARAMETER;

    ID3D11Texture2D_GetDesc(tex, desc);
    if (desc->Usage != D3D11_USAGE_DEFAULT ||
        !(desc->BindFlags & D3D11_BIND_RENDER_TARGET) ||
        desc->ArraySize != 1 || desc->MipLevels != 1 || desc->SampleDesc.Count != 1)
    {
        MP_ERR(ctx, "Expected a single-sample 2D render target with one mip level and slice\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    if (desc->Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc->Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc->Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        MP_ERR(ctx, "Unsupported D3D11 render target format: %u\n", (unsigned)desc->Format);
        return MPV_ERROR_UNSUPPORTED;
    }

    struct priv *p = ctx->priv;
    ID3D11Device *device = ra_d3d11_get_device(p->context->ra_ctx->ra);
    ID3D11Device *texture_device = NULL;
    ID3D11Texture2D_GetDevice(tex, &texture_device);
    bool same_device = texture_device == device;
    SAFE_RELEASE(texture_device);
    SAFE_RELEASE(device);
    if (!same_device) {
        MP_ERR(ctx, "D3D11 render target was created on a different device\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    return 0;
}
#endif

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
#if HAVE_VULKAN || (HAVE_D3D11 && defined(PL_HAVE_D3D11))
    struct priv *p = ctx->priv;
#endif
#if HAVE_VULKAN
    if (p->vk)
        return libmpv_vk_get_target_size(p->vk, params, out_w, out_h);
#endif
#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    if (ra_is_d3d11(p->gpu->ra_ctx->ra)) {
        D3D11_TEXTURE2D_DESC desc;
        int err = get_d3d11_target_desc(ctx, params, &desc);
        if (err < 0)
            return err;
        *out_w = desc.Width;
        *out_h = desc.Height;
        return 0;
    }
#endif
    mpv_opengl_fbo *fbo = get_mpv_render_param(
        params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;
    *out_w = fbo->w;
    *out_h = fbo->h;
    return 0;
}

static bool render_target(struct priv *p, mpv_render_param *params,
                          struct vo_frame *frame, pl_tex tex,
                          struct pl_color_space color, int depth)
{
    bool alpha = p->context->ra_ctx->opts.want_alpha;
    struct pl_frame target_frame = {
        .num_planes = 1,
        .planes[0] = {
            .texture = tex,
            .flipped = GET_MPV_RENDER_PARAM(
                params, MPV_RENDER_PARAM_FLIP_Y, int, 0),
            .components = alpha ? 4 : 3,
            .component_mapping = {0, 1, 2, 3},
        },
        .repr = pl_color_repr_rgb,
        .color = color,
        .crop = {
            .x1 = tex->params.w,
            .y1 = tex->params.h,
        },
    };
    target_frame.repr.alpha = alpha ? PL_ALPHA_INDEPENDENT : PL_ALPHA_NONE;

    return mppl_video_render_frame(p->renderer, frame, &target_frame, depth,
                                   NULL, NULL);
}

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
static int render_d3d11(struct priv *p, mpv_render_param *params,
                        struct vo_frame *frame, int depth)
{
    struct ra *ra = p->context->ra_ctx->ra;
    struct ra_tex *ra_target;
    int err = p->context->fns->wrap_fbo(p->context, params, &ra_target);
    if (err < 0) {
        mppl_video_begin_frame(p->renderer, frame, pl_color_space_srgb, 0);
        mppl_video_skip_frame(p->renderer, frame);
        return err;
    }

    struct pl_color_space color = pl_color_space_srgb;
    if (ra_d3d11_get_format(ra_target->params.format) == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        color.transfer = PL_COLOR_TRC_SCRGB;
        depth = 0;
    }
    mppl_video_begin_frame(p->renderer, frame, color, 0);

    pl_tex tex = gpu_ctx_wrap_ra_tex(p->gpu->gpu, ra, ra_target);
    bool ok = false;
    if (tex) {
        ok = render_target(p, params, frame, tex, color, depth);
        gpu_ctx_unwrap_ra_tex(p->gpu->gpu, ra, &tex);
    } else {
        mppl_video_skip_frame(p->renderer, frame);
    }
    p->context->fns->done_frame(p->context, frame->display_synced);
    return ok ? 0 : MPV_ERROR_GENERIC;
}
#endif

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct priv *p = ctx->priv;
    int depth = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_DEPTH, int, 8);
    if (depth <= 0)
        depth = 8;

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
    if (ra_is_d3d11(p->gpu->ra_ctx->ra))
        return render_d3d11(p, params, frame, depth);
#endif
    mppl_video_begin_frame(p->renderer, frame, pl_color_space_monitor, 0);

#if HAVE_VULKAN
    if (p->vk) {
        struct pl_frame target_frame;
        int err = libmpv_vk_start_frame(p->vk, params, &target_frame);
        if (err < 0) {
            mppl_video_skip_frame(p->renderer, frame);
            return err;
        }
        bool ok = mppl_video_render_frame(p->renderer, frame, &target_frame,
                                          depth, NULL, NULL);
        err = libmpv_vk_end_frame(p->vk, params);
        return err < 0 ? err : ok ? 0 : MPV_ERROR_GENERIC;
    }
#endif

    mpv_opengl_fbo *fbo = get_mpv_render_param(
        params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo) {
        mppl_video_skip_frame(p->renderer, frame);
        return MPV_ERROR_INVALID_PARAMETER;
    }

    struct ra_tex *ra_target;
    int err = p->context->fns->wrap_fbo(p->context, params, &ra_target);
    if (err < 0) {
        mppl_video_skip_frame(p->renderer, frame);
        return err;
    }

    pl_tex target_tex = gpu_ctx_wrap_opengl_fbo(p->gpu, fbo->fbo,
                                                fbo->w, fbo->h);
    if (!target_tex) {
        mppl_video_skip_frame(p->renderer, frame);
        p->context->fns->done_frame(p->context, frame->display_synced);
        return MPV_ERROR_GENERIC;
    }

    bool ok = render_target(p, params, frame, target_tex, pl_color_space_monitor, depth);
    pl_tex_destroy(p->gpu->gpu, &target_tex);
    p->context->fns->done_frame(p->context, frame->display_synced);
    return ok ? 0 : MPV_ERROR_GENERIC;
}

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    struct priv *p = ctx->priv;
    return mppl_video_get_image(p->renderer, imgfmt, w, h, stride_align, flags);
}

static void screenshot(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args)
{
    struct priv *p = ctx->priv;
    mppl_video_screenshot(p->renderer, frame, args);
}

static void perfdata(struct render_backend *ctx,
                     struct voctrl_performance_data *out)
{
    struct priv *p = ctx->priv;
    mppl_video_perfdata(p->renderer, out);
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    if (p->renderer)
        mppl_video_uninit(p->renderer);
    hwdec_devices_destroy(ctx->hwdec_devs);
    ctx->hwdec_devs = NULL;
#if HAVE_VULKAN
    if (p->vk)
        libmpv_vk_destroy(&p->vk);
    else
#endif
        gpu_ctx_destroy(&p->gpu);
    libmpv_gpu_context_destroy(&p->context);
}

const struct render_backend_fns render_backend_gpu_next = {
    .renderer = MPV_RENDER_RENDERER_GPU_NEXT,
    .init = init,
    .check_format = check_format,
    .set_parameter = set_parameter,
    .reconfig = reconfig,
    .reset = reset,
    .set_paused = set_paused,
    .update_external = update_external,
    .resize = resize,
    .get_target_size = get_target_size,
    .render = render,
    .get_image = get_image,
    .screenshot = screenshot,
    .perfdata = perfdata,
    .destroy = destroy,
};
