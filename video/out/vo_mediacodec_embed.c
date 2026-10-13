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

#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
#include <android/native_window_jni.h>

#include "common/common.h"
#include "misc/jni.h"
#include "options/m_option.h"
#include "sub/draw_bmp.h"
#include "vo.h"
#include "video/mp_image.h"
#include "video/hwdec.h"

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;
    int64_t osd_surface;
    ANativeWindow *osd_window;
    struct mp_draw_sub_cache *osd_cache;
    struct mp_osd_res osd_res;
    int64_t osd_id;
    double osd_pts;
    bool osd_redraw;
};

static bool draw_osd(struct vo *vo, bool clear)
{
    struct priv *p = vo->priv;
    if (!p->osd_window)
        return true;

    int w = ANativeWindow_getWidth(p->osd_window);
    int h = ANativeWindow_getHeight(p->osd_window);
    if (w <= 0 || h <= 0)
        return false;
    if (p->osd_res.w != w || p->osd_res.h != h)
        p->osd_id = -1;
    vo->dwidth = w;
    vo->dheight = h;
    struct mp_rect src, dst;
    vo_get_src_dst_rects(vo, &src, &dst, &p->osd_res);
    p->osd_res.w = w;
    p->osd_res.h = h;
    struct sub_bitmap_list *list = clear ? NULL :
        osd_render(vo->osd, p->osd_res, p->osd_pts, 0, mp_draw_sub_formats);
    int64_t id = list ? list->change_id : 0;
    if (p->osd_id == id) {
        talloc_free(list);
        return true;
    }

    struct mp_image *overlay = NULL;
    if (list && list->num_items) {
        struct mp_rect active, modified;
        int num_active, num_modified;
        overlay = mp_draw_sub_overlay(p->osd_cache, list,
            &active, 1, &num_active, &modified, 1, &num_modified);
        if (!overlay) {
            talloc_free(list);
            return false;
        }
    }
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(p->osd_window, &buffer, NULL) < 0) {
        MP_ERR(vo, "Could not lock the OSD Surface.\n");
        talloc_free(list);
        return false;
    }
    if (buffer.format != WINDOW_FORMAT_RGBA_8888 || !buffer.bits ||
        buffer.width <= 0 || buffer.height <= 0 || buffer.stride < buffer.width) {
        ANativeWindow_unlockAndPost(p->osd_window);
        talloc_free(list);
        return false;
    }
    bool valid = buffer.width == w && buffer.height == h;
    for (int y = 0; y < buffer.height; y++) {
        uint8_t *out = (uint8_t *)buffer.bits + (size_t)y * buffer.stride * 4;
        if (!valid || !overlay) {
            memset(out, 0, (size_t)buffer.stride * 4);
            continue;
        }
        const uint8_t *in = overlay->planes[0] + (size_t)y * overlay->stride[0];
        for (int x = 0; x < w; x++) {
            out[4*x+0] = in[4*x+2];
            out[4*x+1] = in[4*x+1];
            out[4*x+2] = in[4*x+0];
            out[4*x+3] = in[4*x+3];
        }
    }
    bool posted = ANativeWindow_unlockAndPost(p->osd_window) == 0;
    if (valid && posted)
        p->osd_id = id;
    talloc_free(list);
    return valid && posted;
}

static AVBufferRef *create_mediacodec_device_ref(struct vo *vo)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    mp_assert(vo->opts->WinID != 0 && vo->opts->WinID != -1);
    hwctx->surface = (void *)(intptr_t)(vo->opts->WinID);

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "mediacodec_embed",
        .av_device_ref = create_mediacodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(vo, "Failed to create hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    p->osd_pts = MP_NOPTS_VALUE;
    p->osd_id = -1;
    if (p->osd_surface) {
        JNIEnv *env = MP_JNI_GET_ENV(vo);
        if (!env || p->osd_surface == -1)
            goto error;
        p->osd_window = ANativeWindow_fromSurface(env, (jobject)(intptr_t)p->osd_surface);
        if (!p->osd_window || ANativeWindow_setBuffersGeometry(p->osd_window,
                0, 0, WINDOW_FORMAT_RGBA_8888) < 0)
            goto error;
        p->osd_cache = mp_draw_sub_alloc(p, vo->global);
        if (!draw_osd(vo, true))
            goto error;
    }
    return 0;

error:
    if (p->osd_window)
        ANativeWindow_release(p->osd_window);
    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    hwdec_devices_destroy(vo->hwdec_devs);
    vo->hwdec_devs = NULL;
    return -1;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->next_image) {
        AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
        av_mediacodec_release_buffer(buffer, 1);
        mp_image_unrefp(&p->next_image);
    }
    draw_osd(vo, p->osd_pts == MP_NOPTS_VALUE);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat)
        mpi = mp_image_new_ref(frame->current);

    talloc_free(p->next_image);
    p->next_image = mpi;
    p->osd_pts = frame->current ? frame->current->pts : MP_NOPTS_VALUE;
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_MEDIACODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    if (!p->osd_window)
        return VO_NOTIMPL;
    switch (request) {
    case VOCTRL_RESET:
        p->osd_pts = MP_NOPTS_VALUE;
        return draw_osd(vo, true) ? VO_TRUE : VO_FALSE;
    case VOCTRL_SET_PANSCAN:
        p->osd_id = -1;
        vo->want_redraw = true;
        return VO_TRUE;
    case VOCTRL_REDRAW:
        // Defer until the VO core has consumed the current redraw request.
        p->osd_redraw = true;
        return VO_TRUE;
    case VOCTRL_CHECK_EVENTS:
        if (p->osd_redraw) {
            p->osd_redraw = false;
            return draw_osd(vo, p->osd_pts == MP_NOPTS_VALUE) ? VO_TRUE : VO_FALSE;
        }
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    p->osd_id = -1;
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);

    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    if (p->osd_window) {
        draw_osd(vo, true);
        ANativeWindow_release(p->osd_window);
    }
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_mediacodec_embed = {
    .description = "Android (Embedded MediaCodec Surface)",
    .name = "mediacodec_embed",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .options_prefix = "mediacodec-embed",
    .options = (const struct m_option[]) {
        {"osd-surface", OPT_INT64(osd_surface)},
        {0}
    },
};
