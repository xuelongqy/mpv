#pragma once

#include <libplacebo/renderer.h>
#include "mpv/render.h"

struct render_backend;
struct gpu_ctx;
struct libmpv_vk;

int libmpv_vk_create(struct render_backend *ctx, mpv_render_param *params,
                    struct libmpv_vk **out, struct gpu_ctx **gpu);
int libmpv_vk_get_target_size(struct libmpv_vk *vk, mpv_render_param *params,
                             int *w, int *h);
int libmpv_vk_start_frame(struct libmpv_vk *vk, mpv_render_param *params,
                         struct pl_frame *target);
int libmpv_vk_end_frame(struct libmpv_vk *vk, mpv_render_param *params);
int libmpv_vk_retire(struct libmpv_vk *vk, void *data);
void libmpv_vk_destroy(struct libmpv_vk **vk);
