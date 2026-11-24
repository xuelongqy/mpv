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

#include <libavcodec/jni.h>
#include <android/native_window_jni.h>

#include "android_common.h"
#include "common/msg.h"
#include "misc/jni.h"
#include "options/m_config.h"
#include "vo.h"
#include "sub/draw_bmp.h"
#include "video/mp_image.h"

struct vo_android_state {
    struct mp_log *log;
    ANativeWindow *native_window;
};

bool vo_android_init(struct vo *vo)
{
    vo->android = talloc_zero(vo, struct vo_android_state);
    struct vo_android_state *ctx = vo->android;

    *ctx = (struct vo_android_state){
        .log = mp_log_new(ctx, vo->log, "android"),
    };

    JNIEnv *env = MP_JNI_GET_ENV(ctx);
    if (!env) {
        MP_FATAL(ctx, "Could not attach java VM.\n");
        goto fail;
    }

    if (vo->opts->WinID == 0 || vo->opts->WinID == -1) {
        MP_FATAL(ctx, "Missing surface pointer\n");
        goto fail;
    }
    jobject surface = (jobject)(intptr_t)vo->opts->WinID;
    ctx->native_window = ANativeWindow_fromSurface(env, surface);
    if (!ctx->native_window) {
        MP_FATAL(ctx, "Failed to create ANativeWindow\n");
        goto fail;
    }

    return true;
fail:
    talloc_free(ctx);
    vo->android = NULL;
    return false;
}

void vo_android_uninit(struct vo *vo)
{
    struct vo_android_state *ctx = vo->android;
    if (!ctx)
        return;

    if (ctx->native_window)
        ANativeWindow_release(ctx->native_window);

    talloc_free(ctx);
    vo->android = NULL;
}

ANativeWindow *vo_android_native_window(struct vo *vo)
{
    struct vo_android_state *ctx = vo->android;
    return ctx->native_window;
}

bool vo_android_surface_size(struct vo *vo, int *out_w, int *out_h)
{
    struct vo_android_state *ctx = vo->android;

    int w = vo->opts->android_surface_size.w,
        h = vo->opts->android_surface_size.h;
    if (!w)
        w = ANativeWindow_getWidth(ctx->native_window);
    if (!h)
        h = ANativeWindow_getHeight(ctx->native_window);

    if (w <= 0 || h <= 0) {
        MP_ERR(ctx, "Failed to get height and width.\n");
        return false;
    }
    *out_w = w;
    *out_h = h;
    return true;
}

bool android_draw_osd(struct mp_log *log, ANativeWindow *window,
                      struct mp_draw_sub_cache *cache, struct osd_state *osd,
                      const struct mp_osd_res *res, double pts, int64_t *prev_id)
{
    int w = res->w, h = res->h;
    struct sub_bitmap_list *list = osd ?
        osd_render(osd, *res, pts, 0, mp_draw_sub_formats) : NULL;
    int64_t id = list ? list->change_id : 0;
    if (*prev_id == id) {
        talloc_free(list);
        return true;
    }

    struct mp_image *overlay = NULL;
    if (list && list->num_items) {
        struct mp_rect active, modified;
        int num_active, num_modified;
        overlay = mp_draw_sub_overlay(cache, list,
            &active, 1, &num_active, &modified, 1, &num_modified);
        if (!overlay) {
            talloc_free(list);
            return false;
        }
    }
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, NULL) < 0) {
        mp_err(log, "Could not lock the OSD Surface.\n");
        talloc_free(list);
        return false;
    }
    if (buffer.format != WINDOW_FORMAT_RGBA_8888 || !buffer.bits ||
        buffer.width <= 0 || buffer.height <= 0 || buffer.stride < buffer.width) {
        ANativeWindow_unlockAndPost(window);
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
    bool posted = ANativeWindow_unlockAndPost(window) == 0;
    if (valid && posted)
        *prev_id = id;
    talloc_free(list);
    return valid && posted;
}
