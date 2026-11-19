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
#include <string.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_d3d11.h>

#include "config.h"

_Static_assert(MPV_RENDER_PARAM_D3D11_INIT_PARAMS == 21, "D3D11 init parameter ABI");
_Static_assert(MPV_RENDER_PARAM_D3D11_FBO == 22, "D3D11 target parameter ABI");

static void check_create(const char *api, const char *renderer, bool include_renderer,
                         int expected)
{
    mpv_handle *mpv = mpv_create();
    if (!mpv)
        exit(1);

    if (mpv_set_option_string(mpv, "vo", "libmpv") < 0 ||
        mpv_initialize(mpv) < 0)
        exit(1);

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)api},
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

static void check_output_modes(void)
{
    mpv_handle *mpv = mpv_create();
    if (!mpv || mpv_set_option_string(mpv, "vo", "libmpv") < 0 ||
        mpv_initialize(mpv) < 0)
        exit(1);

    int advanced = 1;
    const char *modes[] = {NULL, "render", "auto", "native", "invalid", ""};
    for (size_t n = 0; n < sizeof(modes) / sizeof(modes[0]); n++) {
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
            {modes[n] ? MPV_RENDER_PARAM_OUTPUT_MODE : 0, (void *)modes[n]},
            {0},
        };
        mpv_render_context *ctx = NULL;
        int expected = n < 3 ? 0 : n == 3 ? MPV_ERROR_NOT_IMPLEMENTED
                                         : MPV_ERROR_INVALID_PARAMETER;
        int err = mpv_render_context_create(&ctx, mpv, params);
        if (err != expected || (err == 0 && !ctx) || (err < 0 && ctx)) {
            fprintf(stderr, "output mode %s: expected %d, got %d\n",
                    modes[n] ? modes[n] : "(default)", expected, err);
            exit(1);
        }
        if (ctx) {
            const char *actual = NULL;
            err = mpv_render_context_get_info(ctx,
                (mpv_render_param){MPV_RENDER_PARAM_OUTPUT_MODE, &actual});
            if (err < 0 || !actual || strcmp(actual, "render")) {
                fprintf(stderr, "unexpected actual output mode\n");
                exit(1);
            }
            mpv_render_context_free(ctx);
        }
    }
    mpv_d3d11_init_params init = {0};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_D3D11},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &init},
        {0},
    };
    mpv_render_context *ctx = NULL;
    int expected = HAVE_D3D11 ? MPV_ERROR_INVALID_PARAMETER : MPV_ERROR_NOT_IMPLEMENTED;
    int err = mpv_render_context_create(&ctx, mpv, params);
    if (err != expected || ctx) {
        fprintf(stderr, "D3D11 initialization: expected %d, got %d\n", expected, err);
        exit(1);
    }
    mpv_terminate_destroy(mpv);
}

int main(int argc, char **argv)
{
    if (mpv_client_api_version() != MPV_CLIENT_API_VERSION ||
        mpv_client_api_version() < MPV_MAKE_VERSION(2, 8))
        return 1;

    check_create(MPV_RENDER_API_TYPE_SW, NULL, false, 0);
    check_create(MPV_RENDER_API_TYPE_SW, "gpu", true, MPV_ERROR_NOT_IMPLEMENTED);
    check_create(MPV_RENDER_API_TYPE_SW, "gpu-next", true, MPV_ERROR_NOT_IMPLEMENTED);
    check_create(MPV_RENDER_API_TYPE_SW, "unknown", true, MPV_ERROR_INVALID_PARAMETER);
    check_create(MPV_RENDER_API_TYPE_SW, "", true, MPV_ERROR_INVALID_PARAMETER);
    check_create(MPV_RENDER_API_TYPE_SW, NULL, true, MPV_ERROR_INVALID_PARAMETER);

    bool vulkan = argc > 1 && strcmp(argv[1], "vulkan") == 0;
    int missing_init = vulkan ? MPV_ERROR_INVALID_PARAMETER : MPV_ERROR_NOT_IMPLEMENTED;
    check_create(MPV_RENDER_API_TYPE_VULKAN, NULL, false, missing_init);
    check_create(MPV_RENDER_API_TYPE_VULKAN, "gpu-next", true, missing_init);
    check_create(MPV_RENDER_API_TYPE_VULKAN, "gpu", true, MPV_ERROR_NOT_IMPLEMENTED);
    check_create(MPV_RENDER_API_TYPE_VULKAN, "unknown", true, MPV_ERROR_INVALID_PARAMETER);
    check_output_modes();
    return 0;
}
