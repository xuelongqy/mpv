#pragma once

#include <libplacebo/renderer.h>

#include "misc/bstr.h"
#include "video/mp_image.h"
#include "video/out/vo.h"

struct mp_hwdec_devices;
struct mp_log;
struct mp_osd_res;
struct mp_rect;
struct mpv_global;
struct gpu_ctx;
struct osd_state;
struct ra_ctx_opts;

struct gpu_next_renderer;

enum gpu_next_hint_action {
    GPU_NEXT_HINT_KEEP,
    GPU_NEXT_HINT_CLEAR,
    GPU_NEXT_HINT_SET,
};

struct gpu_next_target {
    struct pl_color_space color_space;
    struct pl_color_space hint;
    float reference_luminance;
    int dither_depth;
    bool unknown;
    bool hint_enabled;
    bool external_params;
    enum gpu_next_hint_action hint_action;
};

struct gpu_next_render_result {
    struct mp_image_params target_params;
    struct pl_hdr_metadata hdr;
    bool has_peak;
    bool interpolated;
};

struct gpu_next_renderer *gpu_next_renderer_create(
    void *parent, struct mpv_global *global, struct mp_log *log,
    struct gpu_ctx *context, struct mp_hwdec_devices *hwdec_devs,
    bool load_all_hwdecs);
void gpu_next_renderer_destroy(struct gpu_next_renderer **renderer);
void gpu_next_update_ra_ctx_opts(void *parent, struct mpv_global *global,
                                 struct ra_ctx_opts *opts);

bool gpu_next_renderer_check_format(struct gpu_next_renderer *renderer,
                                    int imgfmt);
struct mp_image *gpu_next_renderer_get_image(struct gpu_next_renderer *renderer,
                                             int imgfmt, int w, int h,
                                             int stride_align, int flags);
void gpu_next_renderer_set_osd(struct gpu_next_renderer *renderer,
                               struct osd_state *osd);
void gpu_next_renderer_config(struct gpu_next_renderer *renderer,
                              const struct mp_image_params *params);
void gpu_next_renderer_resize(struct gpu_next_renderer *renderer,
                              struct mp_rect src, struct mp_rect dst,
                              struct mp_osd_res osd);
void gpu_next_renderer_get_queue_params(struct gpu_next_renderer *renderer,
                                        int *req_frames, int *max_frames);

void gpu_next_renderer_prepare(struct gpu_next_renderer *renderer,
                               struct vo_frame *frame,
                               struct pl_color_space target_csp,
                               float reference_luminance,
                               bool target_hint_supported,
                               struct gpu_next_target *target,
                               struct mp_image_params *hint);
void gpu_next_renderer_skip_frame(struct gpu_next_renderer *renderer,
                                  struct vo_frame *frame);
bool gpu_next_renderer_render(struct gpu_next_renderer *renderer,
                              struct vo_frame *frame,
                              struct pl_frame *target_frame,
                              const struct gpu_next_target *target_state,
                              struct gpu_next_render_result *result);

bool gpu_next_renderer_set_icc_profile(struct gpu_next_renderer *renderer,
                                       bstr icc);
void gpu_next_renderer_reset(struct gpu_next_renderer *renderer);
bool gpu_next_renderer_set_paused(struct gpu_next_renderer *renderer,
                                  bool paused);
void gpu_next_renderer_update_options(struct gpu_next_renderer *renderer);
bool gpu_next_renderer_want_alpha(struct gpu_next_renderer *renderer);
bool gpu_next_renderer_use_auto_icc(struct gpu_next_renderer *renderer);
void gpu_next_renderer_screenshot(struct gpu_next_renderer *renderer,
                                  struct vo_frame *frame,
                                  struct voctrl_screenshot *args);
void gpu_next_renderer_perfdata(struct gpu_next_renderer *renderer,
                                struct voctrl_performance_data *out);
void gpu_next_renderer_load_hwdec(struct gpu_next_renderer *renderer,
                                  struct mp_hwdec_devices *devs, void *data);
