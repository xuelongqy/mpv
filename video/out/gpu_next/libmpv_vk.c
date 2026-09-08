#include <limits.h>
#include <libplacebo/vulkan.h>

#include "common/common.h"
#include "mpv/render_vk.h"
#include "video/out/gpu/context.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "context.h"
#include "libmpv_vk.h"
#include "video.h"

struct target {
    VkImage image;
    uint64_t generation;
    VkFormat format;
    uint32_t width, height;
    VkImageUsageFlags usage;
    pl_tex tex;
};

struct libmpv_vk {
    struct gpu_ctx context;
    struct ra_ctx ra_ctx;
    pl_vulkan vulkan;
    struct target *targets;
    int num_targets;
    pl_tex active;
    bool failed;
};

static bool enabled_features(const VkPhysicalDeviceFeatures2 *features)
{
    bool host_query_reset = false, timeline = false, sync2 = false;
    for (const VkBaseInStructure *s = (const void *)features; s; s = s->pNext) {
        switch (s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            const VkPhysicalDeviceVulkan12Features *f = (const void *)s;
            host_query_reset = f->hostQueryReset;
            timeline = f->timelineSemaphore;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
            host_query_reset =
                ((const VkPhysicalDeviceHostQueryResetFeatures *)s)->hostQueryReset;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            timeline =
                ((const VkPhysicalDeviceTimelineSemaphoreFeatures *)s)->timelineSemaphore;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            sync2 = ((const VkPhysicalDeviceVulkan13Features *)s)->synchronization2;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            sync2 = ((const VkPhysicalDeviceSynchronization2Features *)s)->synchronization2;
            break;
        default: break;
        }
    }
    // The pinned libplacebo's legacy barrier path does not lower NONE stages
    // used when returning external images. Require its synchronization2 path.
    return host_query_reset && timeline && sync2;
}

int libmpv_vk_create(struct render_backend *ctx, mpv_render_param *params,
                    struct libmpv_vk **out, struct gpu_ctx **gpu)
{
    mpv_vulkan_init_params *init = get_mpv_render_param(
        params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!init || init->version != MPV_VULKAN_DRAFT_VERSION ||
        !init->instance || !init->physical_device || !init->device ||
        !init->get_proc_address || !init->features ||
        init->features->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ||
        !init->lock_queue || !init->unlock_queue || init->num_extensions < 0 ||
        (init->num_extensions && !init->extensions))
        return MPV_ERROR_INVALID_PARAMETER;
    bool sync2_ext = false;
    for (int i = 0; i < init->num_extensions; i++) {
        if (!init->extensions[i])
            return MPV_ERROR_INVALID_PARAMETER;
        sync2_ext |= !strcmp(init->extensions[i], VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }
    if (init->instance_api_version < VK_API_VERSION_1_2 ||
        !enabled_features(init->features))
        return MPV_ERROR_UNSUPPORTED;

    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queues = (void *)
        init->get_proc_address(init->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceProperties get_properties = (void *)
        init->get_proc_address(init->instance, "vkGetPhysicalDeviceProperties");
    if (!get_queues || !get_properties)
        return MPV_ERROR_UNSUPPORTED;
    VkPhysicalDeviceProperties properties;
    get_properties(init->physical_device, &properties);
    uint32_t api_version = MPMIN(init->instance_api_version, properties.apiVersion);
    if (api_version < VK_API_VERSION_1_2 ||
        (api_version < VK_API_VERSION_1_3 && !sync2_ext))
        return MPV_ERROR_UNSUPPORTED;
    uint32_t count = 0;
    get_queues(init->physical_device, &count, NULL);
    if (init->queue_family >= count)
        return MPV_ERROR_INVALID_PARAMETER;
    VkQueueFamilyProperties *queues = talloc_array(NULL, VkQueueFamilyProperties, count);
    get_queues(init->physical_device, &count, queues);
    VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    bool usable = queues[init->queue_family].queueCount &&
        (queues[init->queue_family].queueFlags & required) == required;
    talloc_free(queues);
    if (!usable)
        return MPV_ERROR_UNSUPPORTED;

    struct libmpv_vk *p = *out = talloc_zero(ctx->priv, struct libmpv_vk);
    p->ra_ctx = (struct ra_ctx) {.global = ctx->global, .log = ctx->log};
    gpu_next_update_ra_ctx_opts(p, ctx->global, &p->ra_ctx.opts);
    p->context = (struct gpu_ctx) {
        .log = ctx->log,
        .ra_ctx = &p->ra_ctx,
        .pllog = mppl_log_create(p, ctx->log),
    };
    if (!p->context.pllog)
        return MPV_ERROR_UNSUPPORTED;
    p->vulkan = pl_vulkan_import(p->context.pllog, pl_vulkan_import_params(
        .instance = init->instance,
        .phys_device = init->physical_device,
        .device = init->device,
        .get_proc_addr = init->get_proc_address,
        .features = init->features,
        .extensions = init->extensions,
        .num_extensions = init->num_extensions,
        .queue_graphics = {.index = init->queue_family, .count = 1},
        .lock_queue = init->lock_queue,
        .unlock_queue = init->unlock_queue,
        .queue_ctx = init->queue_context,
        .max_api_version = init->instance_api_version,
    ));
    if (!p->vulkan)
        return MPV_ERROR_UNSUPPORTED;
    p->context.gpu = p->vulkan->gpu;
    p->ra_ctx.ra = ra_create_pl(p->context.gpu, ctx->log);
    *gpu = &p->context;
    return 0;
}

static bool valid_layout(VkImageLayout layout, VkImageUsageFlags usage, bool input)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return input;
    case VK_IMAGE_LAYOUT_GENERAL: return true;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return true;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    default: return false;
    }
}

int libmpv_vk_get_target_size(struct libmpv_vk *p, mpv_render_param *params,
                             int *w, int *h)
{
    mpv_vulkan_target *t = get_mpv_render_param(
        params, MPV_RENDER_PARAM_VULKAN_TARGET, NULL);
    if (!t || t->version != MPV_VULKAN_DRAFT_VERSION)
        return MPV_ERROR_INVALID_PARAMETER;
    t->state = MPV_VULKAN_TARGET_UNTOUCHED;
    if (p->failed || pl_gpu_is_failed(p->context.gpu))
        return MPV_ERROR_GENERIC;
    if (!t->image || !t->width || !t->height ||
        t->width > INT_MAX || t->height > INT_MAX ||
        !t->completion || t->completion == t->acquire ||
        (!t->acquire && t->acquire_value) ||
        !(t->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !(t->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !valid_layout(t->input_layout, t->usage, true) ||
        !valid_layout(t->output_layout, t->usage, false))
        return MPV_ERROR_INVALID_PARAMETER;
    if ((t->format != VK_FORMAT_R8G8B8A8_UNORM &&
         t->format != VK_FORMAT_B8G8R8A8_UNORM) ||
        t->width > p->context.gpu->limits.max_tex_2d_dim ||
        t->height > p->context.gpu->limits.max_tex_2d_dim)
        return MPV_ERROR_UNSUPPORTED;
    *w = t->width;
    *h = t->height;
    return 0;
}

int libmpv_vk_start_frame(struct libmpv_vk *p, mpv_render_param *params,
                         struct pl_frame *frame)
{
    int w, h;
    int err = libmpv_vk_get_target_size(p, params, &w, &h);
    if (err < 0)
        return err;
    mpv_vulkan_target *t = get_mpv_render_param(
        params, MPV_RENDER_PARAM_VULKAN_TARGET, NULL);
    struct target *target = NULL;
    for (int i = 0; i < p->num_targets; i++) {
        struct target *s = &p->targets[i];
        if (s->image != t->image)
            continue;
        if (s->generation != t->generation || s->format != t->format ||
            s->width != t->width || s->height != t->height || s->usage != t->usage)
            return MPV_ERROR_INVALID_PARAMETER;
        target = s;
        break;
    }
    if (!target) {
        pl_tex tex = pl_vulkan_wrap(p->context.gpu, pl_vulkan_wrap_params(
            .image = t->image, .width = w, .height = h,
            .format = t->format, .usage = t->usage,
        ));
        if (!tex)
            return MPV_ERROR_UNSUPPORTED;
        struct target new = {
            .image = t->image, .generation = t->generation, .format = t->format,
            .width = t->width, .height = t->height, .usage = t->usage, .tex = tex,
        };
        MP_TARRAY_APPEND(p, p->targets, p->num_targets, new);
        target = &p->targets[p->num_targets - 1];
    }
    p->active = target->tex;
    pl_vulkan_release_ex(p->context.gpu, pl_vulkan_release_params(
        .tex = p->active, .layout = t->input_layout,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = {.sem = t->acquire, .value = t->acquire_value},
    ));
    bool alpha = p->ra_ctx.opts.want_alpha;
    *frame = (struct pl_frame) {
        .num_planes = 1,
        .planes[0] = {
            .texture = p->active,
            .flipped = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_FLIP_Y, int, 0),
            .components = alpha ? 4 : 3,
            .component_mapping = {0, 1, 2, 3},
        },
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_monitor,
        .crop = {.x1 = w, .y1 = h},
    };
    frame->repr.alpha = alpha ? PL_ALPHA_INDEPENDENT : PL_ALPHA_NONE;
    return 0;
}

int libmpv_vk_end_frame(struct libmpv_vk *p, mpv_render_param *params)
{
    mpv_vulkan_target *t = get_mpv_render_param(
        params, MPV_RENDER_PARAM_VULKAN_TARGET, NULL);
    // After a failed render submission, tracked layouts may no longer match
    // the device. Do not submit a return barrier using that stale state.
    bool ok = !pl_gpu_is_failed(p->context.gpu) &&
        pl_vulkan_hold_ex(p->context.gpu, pl_vulkan_hold_params(
            .tex = p->active, .layout = t->output_layout,
            .qf = VK_QUEUE_FAMILY_IGNORED,
            .semaphore = {.sem = t->completion, .value = t->completion_value},
        ));
    p->active = NULL;
    p->failed |= !ok || pl_gpu_is_failed(p->context.gpu);
    t->state = ok ? MPV_VULKAN_TARGET_RETURNED : MPV_VULKAN_TARGET_FAILED;
    return p->failed ? MPV_ERROR_GENERIC : 0;
}

int libmpv_vk_retire(struct libmpv_vk *p, void *data)
{
    mpv_vulkan_retire_target *r = data;
    if (!r || r->version != MPV_VULKAN_DRAFT_VERSION || !r->image)
        return MPV_ERROR_INVALID_PARAMETER;
    if (p->failed || pl_gpu_is_failed(p->context.gpu))
        return MPV_ERROR_GENERIC;
    for (int i = 0; i < p->num_targets; i++) {
        struct target *t = &p->targets[i];
        if (t->image != r->image || t->generation != r->generation)
            continue;
        pl_gpu_finish(p->context.gpu);
        if (pl_gpu_is_failed(p->context.gpu))
            return MPV_ERROR_GENERIC;
        pl_tex_destroy(p->context.gpu, &t->tex);
        MP_TARRAY_REMOVE_AT(p->targets, p->num_targets, i);
        return 0;
    }
    return MPV_ERROR_INVALID_PARAMETER;
}

void libmpv_vk_destroy(struct libmpv_vk **pp)
{
    struct libmpv_vk *p = *pp;
    if (!p)
        return;
    if (p->context.gpu) {
        pl_gpu_finish(p->context.gpu);
        for (int i = 0; i < p->num_targets; i++)
            pl_tex_destroy(p->context.gpu, &p->targets[i].tex);
    }
    // RA_PL's destroy callback frees the RA itself, unlike ra_free().
    if (p->ra_ctx.ra)
        p->ra_ctx.ra->fns->destroy(p->ra_ctx.ra);
    pl_vulkan_destroy(&p->vulkan);
    pl_log_destroy(&p->context.pllog);
    talloc_free(p);
    *pp = NULL;
}
