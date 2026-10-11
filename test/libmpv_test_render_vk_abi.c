/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stddef.h>
#include <stdint.h>
#include <mpv/render_vk.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#define ALIGNOF alignof
#else
#define STATIC_ASSERT _Static_assert
#define ALIGNOF _Alignof
#endif

STATIC_ASSERT(MPV_VULKAN_API_VERSION == 1, "Vulkan v1");
STATIC_ASSERT(MPV_VULKAN_DRAFT_VERSION == MPV_VULKAN_API_VERSION, "draft alias");
STATIC_ASSERT(MPV_RENDER_PARAM_VULKAN_INIT_PARAMS == 22, "init parameter");
STATIC_ASSERT(MPV_RENDER_PARAM_VULKAN_TARGET == 23, "target parameter");
STATIC_ASSERT(MPV_RENDER_PARAM_VULKAN_RETIRE_TARGET == 24, "retire parameter");
STATIC_ASSERT(MPV_VULKAN_TARGET_UNTOUCHED == 0, "untouched state");
STATIC_ASSERT(MPV_VULKAN_TARGET_RETURNED == 1, "returned state");
STATIC_ASSERT(MPV_VULKAN_TARGET_FAILED == 2, "failed state");

// Freeze the validated 64-bit C ABI, not a layout shared with untested 32-bit ABIs.
#if UINTPTR_MAX == UINT64_MAX
#define OFFSET(type, field, offset) \
    STATIC_ASSERT(offsetof(type, field) == offset, #type "." #field)

STATIC_ASSERT(sizeof(mpv_vulkan_target_state) == 4, "state size");
STATIC_ASSERT(sizeof(mpv_vulkan_init_params) == 88, "init size");
STATIC_ASSERT(ALIGNOF(mpv_vulkan_init_params) == 8, "init alignment");
OFFSET(mpv_vulkan_init_params, version, 0);
OFFSET(mpv_vulkan_init_params, instance_api_version, 4);
OFFSET(mpv_vulkan_init_params, instance, 8);
OFFSET(mpv_vulkan_init_params, physical_device, 16);
OFFSET(mpv_vulkan_init_params, device, 24);
OFFSET(mpv_vulkan_init_params, get_proc_address, 32);
OFFSET(mpv_vulkan_init_params, features, 40);
OFFSET(mpv_vulkan_init_params, extensions, 48);
OFFSET(mpv_vulkan_init_params, num_extensions, 56);
OFFSET(mpv_vulkan_init_params, queue_family, 60);
OFFSET(mpv_vulkan_init_params, lock_queue, 64);
OFFSET(mpv_vulkan_init_params, unlock_queue, 72);
OFFSET(mpv_vulkan_init_params, queue_context, 80);

STATIC_ASSERT(sizeof(mpv_vulkan_target) == 88, "target size");
STATIC_ASSERT(ALIGNOF(mpv_vulkan_target) == 8, "target alignment");
OFFSET(mpv_vulkan_target, version, 0);
OFFSET(mpv_vulkan_target, image, 8);
OFFSET(mpv_vulkan_target, generation, 16);
OFFSET(mpv_vulkan_target, format, 24);
OFFSET(mpv_vulkan_target, width, 28);
OFFSET(mpv_vulkan_target, height, 32);
OFFSET(mpv_vulkan_target, usage, 36);
OFFSET(mpv_vulkan_target, input_layout, 40);
OFFSET(mpv_vulkan_target, output_layout, 44);
OFFSET(mpv_vulkan_target, acquire, 48);
OFFSET(mpv_vulkan_target, acquire_value, 56);
OFFSET(mpv_vulkan_target, completion, 64);
OFFSET(mpv_vulkan_target, completion_value, 72);
OFFSET(mpv_vulkan_target, state, 80);

STATIC_ASSERT(sizeof(mpv_vulkan_retire_target) == 24, "retire size");
STATIC_ASSERT(ALIGNOF(mpv_vulkan_retire_target) == 8, "retire alignment");
OFFSET(mpv_vulkan_retire_target, version, 0);
OFFSET(mpv_vulkan_retire_target, image, 8);
OFFSET(mpv_vulkan_retire_target, generation, 16);
#endif

int main(void)
{
    return UINTPTR_MAX == UINT64_MAX ? 0 : 77;
}
