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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpv/client.h>
#include <mpv/render.h>

static mpv_handle *mpv;
static mpv_render_context *render_ctx;

static void cleanup(void)
{
    mpv_render_context_free(render_ctx);
    render_ctx = NULL;
    mpv_terminate_destroy(mpv);
    mpv = NULL;
}

static void fail(const char *message, int error)
{
    if (error < 0)
        fprintf(stderr, "%s: %s\n", message, mpv_error_string(error));
    else
        fprintf(stderr, "%s\n", message);
    exit(1);
}

static void set_option(const char *name, const char *value)
{
    int error = mpv_set_option_string(mpv, name, value);
    if (error < 0)
        fail("could not set option", error);
}

static bool pixels_changed(const uint32_t *pixels, size_t count)
{
    for (size_t n = 0; n < count; n++) {
        if (pixels[n] != UINT32_C(0xA5A5A5A5))
            return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
        return 1;

    mpv = mpv_create();
    if (!mpv)
        return 1;
    atexit(cleanup);

    set_option("vo", "libmpv");
    set_option("ao", "null");
    set_option("pause", "yes");

    int error = mpv_initialize(mpv);
    if (error < 0)
        fail("could not initialize mpv", error);

    char api[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param create_params[] = {
        {MPV_RENDER_PARAM_API_TYPE, api},
        {0},
    };
    error = mpv_render_context_create(&render_ctx, mpv, create_params);
    if (error < 0)
        fail("could not create software render context", error);

    const char *command[] = {"loadfile", argv[1], NULL};
    error = mpv_command(mpv, command);
    if (error < 0)
        fail("could not load test image", error);

    enum { width = 64, height = 64 };
    uint32_t pixels[width * height];
    int size[] = {width, height};
    char format[] = "rgb0";
    size_t stride = width * sizeof(pixels[0]);
    int block = 0;
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, format},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, pixels},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {0},
    };

    bool loaded = false;
    bool configured = false;
    bool rendered = false;
    int64_t deadline = mpv_get_time_ns(mpv) + 10 * INT64_C(1000000000);

    while (!rendered && mpv_get_time_ns(mpv) < deadline) {
        uint64_t update = mpv_render_context_update(render_ctx);
        if (configured && (update & MPV_RENDER_UPDATE_FRAME)) {
            memset(pixels, 0xA5, sizeof(pixels));
            error = mpv_render_context_render(render_ctx, render_params);
            if (error < 0)
                fail("software rendering failed", error);
            mpv_render_context_report_swap(render_ctx);
            rendered = pixels_changed(pixels, width * height);
        }

        mpv_event *event = mpv_wait_event(mpv, 0.05);
        if (event->event_id == MPV_EVENT_FILE_LOADED)
            loaded = true;
        if (event->event_id == MPV_EVENT_VIDEO_RECONFIG)
            configured = true;
        if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *end = event->data;
            if (end->reason == MPV_END_FILE_REASON_ERROR)
                fail("test image playback failed", end->error);
        }
    }

    if (!loaded)
        fail("test image was not loaded", 0);
    if (!rendered)
        fail("software renderer did not change the target pixels", 0);

    return 0;
}
