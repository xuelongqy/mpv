/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <mpv/client.h>
#include <mpv/render.h>

static void check_create(const char *renderer, bool include_renderer,
                         int expected)
{
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        exit(1);

    if (mpv_set_option_string(mpv, "vo", "libmpv") < 0 ||
        mpv_initialize(mpv) < 0)
        exit(1);

    char api[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, api},
        {include_renderer ? MPV_RENDER_PARAM_RENDERER : 0, (void *)renderer},
        {0},
    };
    mpv_render_context *ctx = NULL;
    int err = mpv_render_context_create(&ctx, mpv, params);
    if (err != expected) {
        fprintf(stderr, "renderer %s: expected %d, got %d\n",
                renderer ? renderer : "(null)", expected, err);
        exit(1);
    }

    mpv_render_context_free(ctx);
    mpv_terminate_destroy(mpv);
}

int main(void)
{
    check_create(NULL, false, 0);
    check_create("gpu", true, MPV_ERROR_NOT_IMPLEMENTED);
    check_create("gpu-next", true, MPV_ERROR_NOT_IMPLEMENTED);
    check_create("unknown", true, MPV_ERROR_INVALID_PARAMETER);
    check_create("", true, MPV_ERROR_INVALID_PARAMETER);
    check_create(NULL, true, MPV_ERROR_INVALID_PARAMETER);
    return 0;
}
