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

#pragma once

#include <android/native_window_jni.h>

#include "common/common.h"

struct vo;
struct mp_draw_sub_cache;
struct mp_log;
struct osd_state;
struct mp_osd_res;

bool vo_android_init(struct vo *vo);
void vo_android_uninit(struct vo *vo);
ANativeWindow *vo_android_native_window(struct vo *vo);
bool vo_android_surface_size(struct vo *vo, int *w, int *h);
bool android_draw_osd(struct mp_log *log, ANativeWindow *window,
                      struct mp_draw_sub_cache *cache, struct osd_state *osd,
                      const struct mp_osd_res *res, double pts, int64_t *prev_id);
