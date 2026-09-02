/*
 * Copyright (C) 2021 Niklas Haas
 *
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
#include <libplacebo/swapchain.h>

#include "common/common.h"
#include "options/m_config.h"
#include "sub/osd.h"
#include "video/out/gpu/context.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/gpu_next/context.h"
#include "video/out/gpu_next/video.h"
#include "video/out/vo.h"

struct priv {
    struct gpu_ctx *context;
    struct ra_ctx *ra_ctx;
    struct gpu_next_renderer *renderer;
    struct mp_image_params target_params;
    bool frame_pending;
};

static void uninit(struct vo *vo);

static void load_hwdec_api(void *ctx, struct hwdec_imgfmt_request *params)
{
    vo_control(ctx, VOCTRL_LOAD_HWDEC_API, params);
}

static void update_queue(struct vo *vo)
{
    struct priv *p = vo->priv;
    int req_frames, max_frames;
    gpu_next_renderer_get_queue_params(p->renderer, &req_frames, &max_frames);
    vo_set_queue_params(vo, 0, req_frames, max_frames);
}

static void resize(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    if (vo->dwidth && vo->dheight) {
        gpu_ctx_resize(p->context, vo->dwidth, vo->dheight);
        vo->want_redraw = true;
    }
    gpu_next_renderer_resize(p->renderer, src, dst, osd);
}

static bool update_auto_profile(struct vo *vo, int *events)
{
    struct priv *p = vo->priv;
    if (!gpu_next_renderer_use_auto_icc(p->renderer))
        return false;

    MP_VERBOSE(vo, "Querying ICC profile...\n");
    bstr icc = {0};
    int r = p->ra_ctx->fns->control(p->ra_ctx, events,
                                    VOCTRL_GET_ICC_PROFILE, &icc);
    if (r == VO_NOTAVAIL)
        return false;
    if (r == VO_FALSE)
        MP_WARN(vo, "Could not retrieve an ICC profile.\n");
    if (r == VO_NOTIMPL)
        MP_ERR(vo, "icc-profile-auto not implemented on this platform.\n");
    gpu_next_renderer_set_icc_profile(p->renderer, icc);
    return true;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct ra_ctx_opts *ctx_opts =
        mp_get_config_group(vo, vo->global, &ra_ctx_conf);
    gpu_next_update_ra_ctx_opts(vo, vo->global, ctx_opts);
    p->context = gpu_ctx_create(vo, ctx_opts);
    talloc_free(ctx_opts);
    if (!p->context)
        goto error;
    p->ra_ctx = p->context->ra_ctx;

    vo->hwdec_devs = hwdec_devices_create();
    hwdec_devices_set_loader(vo->hwdec_devs, load_hwdec_api, vo);
    p->renderer = gpu_next_renderer_create(p, vo->global, vo->log, p->context,
                                           vo->hwdec_devs, false);
    if (!p->renderer)
        goto error;

    gpu_next_renderer_set_osd(p->renderer, vo->osd);
    update_queue(vo);
    return 0;

error:
    uninit(vo);
    return -1;
}

static int query_format(struct vo *vo, int format)
{
    struct priv *p = vo->priv;
    return gpu_next_renderer_check_format(p->renderer, format);
}

static struct mp_image *get_image(struct vo *vo, int imgfmt, int w, int h,
                                  int stride_align, int flags)
{
    struct priv *p = vo->priv;
    return gpu_next_renderer_get_image(p->renderer, imgfmt, w, h,
                                       stride_align, flags);
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    if (!p->ra_ctx->fns->reconfig(p->ra_ctx))
        return -1;

    if (params)
        gpu_next_renderer_config(p->renderer, params);
    resize(vo);
    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = NULL;
    mp_mutex_unlock(&vo->params_mutex);
    return 0;
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    struct pl_color_space target_csp = {0};
    if (sw->fns->target_csp)
        target_csp = sw->fns->target_csp(sw);

    float reference_luminance = 0;
#if PL_API_VER >= 371
    if (sw->fns->target_ref_luma)
        reference_luminance = sw->fns->target_ref_luma(sw);
#endif

    struct gpu_next_target target;
    struct mp_image_params hint;
    gpu_next_renderer_prepare(p->renderer, frame, target_csp,
                              reference_luminance, true, &target, &hint);
    if (sw->fns->color_depth)
        target.dither_depth = sw->fns->color_depth(sw);

    if (target.hint_action != GPU_NEXT_HINT_KEEP) {
        struct mp_image_params *params =
            target.hint_action == GPU_NEXT_HINT_SET ? &hint : NULL;
        if (sw->fns->set_color && sw->fns->set_color(sw, params) && params) {
            target.hint = params->color;
            target.external_params = true;
        }
        if (!target.external_params)
            pl_swapchain_colorspace_hint(p->context->swapchain,
                                         params ? &target.hint : NULL);
    }

    struct pl_swapchain_frame swframe;
    if (!sw->fns->start_frame(sw, NULL) ||
        !pl_swapchain_start_frame(p->context->swapchain, &swframe))
    {
        gpu_next_renderer_skip_frame(p->renderer, frame);
        return VO_FALSE;
    }

    struct pl_frame target_frame;
    pl_frame_from_swapchain(&target_frame, &swframe);
    struct gpu_next_render_result result = {0};
    bool ok = gpu_next_renderer_render(p->renderer, frame, &target_frame, &target,
                                       &result);
    p->frame_pending = true;

    if (ok) {
        mp_mutex_lock(&vo->params_mutex);
        p->target_params = result.target_params;
        vo->target_params = &p->target_params;
        if (vo->params) {
            vo->params->color.hdr = result.hdr;
            vo->has_peak_detect_values = result.has_peak;
        }
        mp_mutex_unlock(&vo->params_mutex);
    }
    return VO_TRUE;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->frame_pending) {
        if (!pl_swapchain_submit_frame(p->context->swapchain))
            MP_ERR(vo, "Failed presenting frame!\n");
        p->frame_pending = false;
    }
    p->ra_ctx->swapchain->fns->swap_buffers(p->ra_ctx->swapchain);
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    if (sw->fns->get_vsync)
        sw->fns->get_vsync(sw, info);
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    switch (request) {
    case VOCTRL_SET_PANSCAN:
        resize(vo);
        return VO_TRUE;
    case VOCTRL_PAUSE:
        if (gpu_next_renderer_set_paused(p->renderer, true))
            vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_RESUME:
        gpu_next_renderer_set_paused(p->renderer, false);
        return VO_TRUE;
    case VOCTRL_UPDATE_RENDER_OPTS: {
        gpu_next_renderer_update_options(p->renderer);
        p->ra_ctx->opts.want_alpha =
            gpu_next_renderer_want_alpha(p->renderer);
        if (p->ra_ctx->fns->update_render_opts)
            p->ra_ctx->fns->update_render_opts(p->ra_ctx);
        update_queue(vo);
        vo->want_redraw = true;
        int events = 0;
        update_auto_profile(vo, &events);
        vo_event(vo, events);
        return VO_TRUE;
    }
    case VOCTRL_RESET:
        gpu_next_renderer_reset(p->renderer);
        return VO_TRUE;
    case VOCTRL_PERFORMANCE_DATA:
        gpu_next_renderer_perfdata(p->renderer, data);
        return VO_TRUE;
    case VOCTRL_SCREENSHOT:
        gpu_next_renderer_screenshot(p->renderer, NULL, data);
        return VO_TRUE;
    case VOCTRL_EXTERNAL_RESIZE:
        return reconfig(vo, NULL) < 0 ? VO_FALSE : VO_TRUE;
    case VOCTRL_LOAD_HWDEC_API:
        gpu_next_renderer_load_hwdec(p->renderer, vo->hwdec_devs, data);
        return VO_TRUE;
    }

    int events = 0;
    int r = p->ra_ctx->fns->control(p->ra_ctx, &events, request, data);
    if ((events & VO_EVENT_ICC_PROFILE_CHANGED) &&
        update_auto_profile(vo, &events))
        vo->want_redraw = true;
    if (events & VO_EVENT_RESIZE)
        resize(vo);
    if (events & VO_EVENT_EXPOSE)
        vo->want_redraw = true;
    vo_event(vo, events);
    return r;
}

static void wakeup(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wakeup)
        p->ra_ctx->fns->wakeup(p->ra_ctx);
}

static void wait_events(struct vo *vo, int64_t until_time_ns)
{
    struct priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wait_events)
        p->ra_ctx->fns->wait_events(p->ra_ctx, until_time_ns);
    else
        vo_wait_default(vo, until_time_ns);
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    gpu_next_renderer_destroy(&p->renderer);
    if (vo->hwdec_devs) {
        hwdec_devices_set_loader(vo->hwdec_devs, NULL, NULL);
        hwdec_devices_destroy(vo->hwdec_devs);
        vo->hwdec_devs = NULL;
    }
    p->ra_ctx = NULL;
    gpu_ctx_destroy(&p->context);
}

const struct vo_driver video_out_gpu_next = {
    .description = "Video output based on libplacebo",
    .name = "gpu-next",
    .caps = VO_CAP_ROTATE90 | VO_CAP_FILM_GRAIN | VO_CAP_VFLIP,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .get_image_ts = get_image,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .get_vsync = get_vsync,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
