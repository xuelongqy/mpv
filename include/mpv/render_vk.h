/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Experimental Vulkan Render API draft. Not installed or ABI-stable.
 * This header must be used with the matching experimental libmpv build.
 * mpv_client_api_version() does not identify revisions of this draft.
 */
#ifndef MPV_CLIENT_API_RENDER_VK_H_
#define MPV_CLIENT_API_RENDER_VK_H_

#include <stdint.h>
#include <vulkan/vulkan.h>
#include "render.h"

#define MPV_VULKAN_DRAFT_VERSION 1

/**
 * MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, for context creation only.
 * Select API "vulkan" and renderer "gpu-next". No window or swapchain is
 * created by libmpv. Only software-decoded input and SDR output are validated.
 *
 * The caller owns all Vulkan handles. The instance and device must outlive
 * the render context. All Render API calls must be externally serialized,
 * as for other backends, but no thread-current graphics context is required.
 *
 * Vulkan 1.2 is required. features is the chain actually ENABLED when creating
 * device, not a supported-features query. It must enable hostQueryReset and
 * timelineSemaphore (Vulkan12Features or the corresponding extension structs).
 * This draft also requires synchronization2, including VK_KHR_synchronization2
 * on Vulkan 1.2, because the pinned libplacebo's legacy barrier lowering cannot
 * handle the NONE stages used for returning external images.
 * extensions likewise lists the device extensions actually enabled.
 *
 * Only queue 0 of queue_family is imported. It must have been created and must
 * support both graphics and compute. Images must be owned by this family (or
 * concurrently shared with it); transfers to other families are not supported
 * by this draft.
 *
 * Both callbacks are required and must lock the same mutex as the caller's
 * queue submissions/presentation. They may be invoked from different threads.
 * Never hold this mutex while calling mpv_render_*(), and never call mpv from
 * these callbacks. The callback context must outlive the render context.
 * Other input pointers need only remain valid during create().
 */
typedef struct mpv_vulkan_init_params {
    uint32_t version;
    uint32_t instance_api_version;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    PFN_vkGetInstanceProcAddr get_proc_address;
    const VkPhysicalDeviceFeatures2 *features;
    const char *const *extensions;
    int num_extensions;
    uint32_t queue_family;
    void (*lock_queue)(void *ctx, uint32_t family, uint32_t index);
    void (*unlock_queue)(void *ctx, uint32_t family, uint32_t index);
    void *queue_context;
} mpv_vulkan_init_params;

typedef enum mpv_vulkan_target_state {
    // libmpv did not take the image or consume acquire. No completion signal.
    MPV_VULKAN_TARGET_UNTOUCHED = 0,
    // Ownership was returned. Wait for completion, even if render() failed.
    // A render error means image contents must not be presented as a valid frame.
    MPV_VULKAN_TARGET_RETURNED = 1,
    // Return submission failed. Completion is NOT guaranteed to signal.
    // Stop using the context and destroy it before recovering the caller's
    // device/resources. Image contents and layout are unspecified.
    MPV_VULKAN_TARGET_FAILED = 2,
} mpv_vulkan_target_state;

/**
 * MPV_RENDER_PARAM_VULKAN_TARGET, for render() only. This structure is in/out.
 * Initialize state to UNTOUCHED before EVERY call, including SKIP_RENDERING:
 * the common Render API can return without invoking the backend.
 *
 * image belongs to device from init. It is a bound, optimal-tiled, 2D color
 * image, one mip, one layer, VK_SAMPLE_COUNT_1_BIT. Only R8G8B8A8_UNORM and
 * B8G8R8A8_UNORM are supported. usage must match creation and include
 * COLOR_ATTACHMENT_BIT and TRANSFER_DST_BIT (background/error clears).
 * The caller must query support before creating images.
 * No multisampling, external memory import or ownership transfer is performed.
 *
 * A wrapper is cached per image. generation is a caller-assigned identity
 * (e.g. swapchain generation). Neither it nor the image description may change
 * until explicitly retired. Do not destroy an image while it is registered.
 * Do not register the same image in another render context at the same time.
 *
 * acquire is optional only if prior access is already complete. input_layout
 * describes the image when acquire signals; UNDEFINED discards old contents.
 * completion is required and must differ from acquire. A value of zero denotes
 * a binary semaphore; nonzero values denote a timeline semaphore. The declared
 * kind must match creation. Signal values must obey Vulkan monotonicity rules.
 * Never reuse a binary signal before its previous signal has been consumed.
 * All supplied semaphores must remain alive until context destruction AND
 * completion of the caller's GPU operations. This conservative draft lifetime
 * accommodates libplacebo's deferred semaphore references.
 *
 * render() returning is NOT GPU completion. Consult state even on failure;
 * only RETURNED guarantees the completion signal and output_layout transition.
 * On success, the caller waits for completion before accessing/presenting image.
 * SKIP_RENDERING ignores this parameter and does not touch synchronization.
 *
 * Targets default to SDR monitor/sRGB. FLIP_Y and DEPTH retain their existing
 * meanings (omitted/nonpositive depth is 8). HDR surfaces are not validated.
 */
typedef struct mpv_vulkan_target {
    uint32_t version;
    VkImage image;
    uint64_t generation;
    VkFormat format;
    uint32_t width;
    uint32_t height;
    VkImageUsageFlags usage;
    VkImageLayout input_layout;
    VkImageLayout output_layout;
    VkSemaphore acquire;
    uint64_t acquire_value;
    VkSemaphore completion;
    uint64_t completion_value;
    mpv_vulkan_target_state state;
} mpv_vulkan_target;

/**
 * MPV_RENDER_PARAM_VULKAN_RETIRE_TARGET, for set_parameter() only.
 * Must match a registered image/generation. Waits for libmpv GPU work, then
 * removes its wrapper without destroying image. May block; use on resize or
 * target disposal, not every frame. The caller must also finish its own access
 * and presentation before destroying the image or its swapchain.
 * Context destruction retires all remaining wrappers. The caller must stop
 * submitting new work involving them before context destruction.
 */
typedef struct mpv_vulkan_retire_target {
    uint32_t version;
    VkImage image;
    uint64_t generation;
} mpv_vulkan_retire_target;

#endif
