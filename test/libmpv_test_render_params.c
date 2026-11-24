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
#if defined(__APPLE__)
#include <mpv/render_apple.h>
#endif
#if defined(__ANDROID__)
#include <mpv/render_android.h>
#endif

#include "config.h"

_Static_assert(MPV_RENDER_PARAM_D3D11_INIT_PARAMS == 21, "D3D11 init parameter ABI");
_Static_assert(MPV_RENDER_PARAM_D3D11_FBO == 22, "D3D11 target parameter ABI");
_Static_assert(MPV_RENDER_PARAM_OUTPUT_MODE == 25, "Output mode parameter ABI");
_Static_assert(MPV_RENDER_PARAM_APPLE_NATIVE_INIT_PARAMS == 26, "Apple init parameter ABI");
_Static_assert(MPV_RENDER_PARAM_APPLE_NATIVE_TARGET == 27, "Apple target parameter ABI");
_Static_assert(MPV_RENDER_PARAM_ANDROID_NATIVE_INIT_PARAMS == 28, "Android init parameter ABI");
_Static_assert(MPV_RENDER_PARAM_ANDROID_NATIVE_TARGET == 29, "Android target parameter ABI");
_Static_assert(MPV_RENDER_PARAM_DISPLAY_HDR10_CAPABLE == 30, "Display HDR10 parameter ABI");
#if defined(__APPLE__)
_Static_assert(MPV_APPLE_NATIVE_API_VERSION == 1, "Apple native API version");
#endif
#if defined(__ANDROID__)
_Static_assert(MPV_ANDROID_NATIVE_API_VERSION == 1, "Android native API version");
#endif

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
    const char *modes[] = {NULL, "render", "auto", "native", "invalid", "", NULL};
    for (size_t n = 0; n < sizeof(modes) / sizeof(modes[0]); n++) {
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
            {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
            {n ? MPV_RENDER_PARAM_OUTPUT_MODE : 0, (void *)modes[n]},
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
            if (mpv_render_context_get_info(ctx,
                    (mpv_render_param){MPV_RENDER_PARAM_OUTPUT_MODE, NULL}) !=
                MPV_ERROR_INVALID_PARAMETER)
                exit(1);
            int capable = 1;
            mpv_render_param display = {MPV_RENDER_PARAM_DISPLAY_HDR10_CAPABLE, &capable};
            if (mpv_render_context_set_parameter(ctx, display) < 0)
                exit(1);
            capable = 2;
            if (mpv_render_context_set_parameter(ctx, display) != MPV_ERROR_INVALID_PARAMETER)
                exit(1);
            mpv_render_context_free(ctx);
        }
    }
    mpv_render_param auto_without_advanced[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
        {MPV_RENDER_PARAM_OUTPUT_MODE, "auto"},
        {0},
    };
    mpv_render_context *ctx = NULL;
    if (mpv_render_context_create(&ctx, mpv, auto_without_advanced) !=
        MPV_ERROR_INVALID_PARAMETER || ctx)
        exit(1);

    mpv_d3d11_init_params init = {0};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_D3D11},
        {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &init},
        {0},
    };
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
        mpv_client_api_version() < MPV_MAKE_VERSION(2, 9))
        return 1;

    check_create(MPV_RENDER_API_TYPE_SW, NULL, false, 0);
    check_create(MPV_RENDER_API_TYPE_SW, "gpu", true, MPV_ERROR_NOT_IMPLEMENTED);
    check_create(MPV_RENDER_API_TYPE_SW, "gpu-next", true, MPV_ERROR_NOT_IMPLEMENTED);
    check_create(MPV_RENDER_API_TYPE_SW, "unknown", true, MPV_ERROR_INVALID_PARAMETER);
    check_create(MPV_RENDER_API_TYPE_SW, "", true, MPV_ERROR_INVALID_PARAMETER);
    check_create(MPV_RENDER_API_TYPE_SW, NULL, true, MPV_ERROR_INVALID_PARAMETER);
    check_create(MPV_RENDER_API_TYPE_APPLE_NATIVE, NULL, false,
                 HAVE_APPLE_NATIVE ? MPV_ERROR_INVALID_PARAMETER : MPV_ERROR_NOT_IMPLEMENTED);
    check_create(MPV_RENDER_API_TYPE_ANDROID_NATIVE, NULL, false,
                 HAVE_ANDROID ? MPV_ERROR_INVALID_PARAMETER : MPV_ERROR_NOT_IMPLEMENTED);

    bool vulkan = argc > 1 && strcmp(argv[1], "vulkan") == 0;
    int missing_init = vulkan ? MPV_ERROR_INVALID_PARAMETER : MPV_ERROR_NOT_IMPLEMENTED;
    check_create(MPV_RENDER_API_TYPE_VULKAN, NULL, false, missing_init);
    check_create(MPV_RENDER_API_TYPE_VULKAN, "gpu-next", true, missing_init);
    check_create(MPV_RENDER_API_TYPE_VULKAN, "gpu", true, MPV_ERROR_NOT_IMPLEMENTED);
    check_create(MPV_RENDER_API_TYPE_VULKAN, "unknown", true, MPV_ERROR_INVALID_PARAMETER);
    check_output_modes();
    return 0;
}
