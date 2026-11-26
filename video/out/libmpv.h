#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "mpv/render.h"
#include "vo.h"

// Helper for finding a parameter value. It returns the direct pointer to the
// value, and if not present, just returns the def argument. In particular, if
// def is not NULL, this never returns NULL (unless a param value is defined
// as accepting NULL, or the libmpv API user is triggering UB).
void *get_mpv_render_param(mpv_render_param *params, mpv_render_param_type type,
                           void *def);

#define GET_MPV_RENDER_PARAM(params, type, ctype, def) \
    (*(ctype *)get_mpv_render_param(params, type, &(ctype){(def)}))

typedef int (*mp_render_cb_control_fn)(struct vo *vo, void *cb_ctx, int *events,
                                       uint32_t request, void *data);
void mp_render_context_set_control_callback(mpv_render_context *ctx,
                                            mp_render_cb_control_fn callback,
                                            void *callback_ctx);
bool mp_render_context_acquire(mpv_render_context *ctx);

enum native_video {
    NATIVE_VIDEO_GENERIC,
    NATIVE_VIDEO_HDR10,
    NATIVE_VIDEO_DV,
    NATIVE_VIDEO_UNSUPPORTED,
};

enum native_policy {
    NATIVE_EXPLICIT,
    NATIVE_AUTO_WITH_FALLBACK,
    NATIVE_AUTO_WITHOUT_FALLBACK,
};

struct mp_codec_params;

struct render_backend {
    struct mpv_global *global;
    struct mp_log *log;
    const struct render_backend_fns *fns;

    // Set on init, immutable afterwards.
    int driver_caps;
    struct mp_hwdec_devices *hwdec_devs;

    enum native_video native_video; // Snapshot for this render call.
    void *priv;
};

// Generic backend for rendering via libmpv. This corresponds to vo/vo_driver,
// except for rendering via the mpv_render_*() API. (As a consequence it's as
// generic as the VO API.) Like with VOs, one backend can support multiple
// underlying GPU APIs.
struct render_backend_fns {
    // Returns libmpv error code. In particular, this function has to check for
    // MPV_RENDER_PARAM_API_TYPE, and silently return MPV_ERROR_NOT_IMPLEMENTED
    // if the API is not included in this backend.
    // If this fails, ->destroy() will be called.
    int (*init)(struct render_backend *ctx, mpv_render_param *params);
    // Optional stream selection for native backends. Display HDR10 capability
    // is -1 if unspecified, 0 if unsupported, or 1 if supported.
    enum native_video (*select_native)(struct render_backend *ctx,
        const struct mp_codec_params *codec, enum native_policy policy, int display_hdr10);
    // Check if the passed IMGFMT_ is supported.
    bool (*check_format)(struct render_backend *ctx, int imgfmt);
    // Implementation of mpv_render_context_set_parameter(). Optional.
    int (*set_parameter)(struct render_backend *ctx, mpv_render_param param);
    // Like vo_driver.reconfig().
    void (*reconfig)(struct render_backend *ctx, struct mp_image_params *params);
    // Like VOCTRL_RESET.
    void (*reset)(struct render_backend *ctx);
    void (*screenshot)(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args);
    void (*perfdata)(struct render_backend *ctx,
                     struct voctrl_performance_data *out);
    // Like vo_driver.get_image().
    struct mp_image *(*get_image)(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags);
    // This has two purposes: 1. set queue attributes on VO, 2. update the
    // renderer's OSD pointer. Keep in mind that as soon as the caller releases
    // the renderer lock, the VO pointer can become invalid. The OSD pointer
    // will technically remain valid (even though it's a vo field), until it's
    // unset with this function.
    // Will be called if vo changes, or if renderer options change.
    void (*update_external)(struct render_backend *ctx, struct vo *vo);
    // Update screen area.
    void (*resize)(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd);
    // Get target surface size from mpv_render_context_render() arguments.
    int (*get_target_size)(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h);
    // Implementation of mpv_render_context_render().
    int (*render)(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame);
    // Optional native presentation after the target time. Discard the sample
    // and its output when the stream changed during render().
    int (*present)(struct render_backend *ctx, mpv_render_param *params, bool display);
    // Free all data in ctx->priv.
    void (*destroy)(struct render_backend *ctx);
};

extern const struct render_backend_fns render_backend_gpu;
extern const struct render_backend_fns render_backend_gpu_next;
extern const struct render_backend_fns render_backend_sw;
extern const struct render_backend_fns render_backend_apple;
extern const struct render_backend_fns render_backend_android;
bool mp_native_hdr10_codec(const struct mp_codec_params *codec);
struct AVCodecParameters;
struct AVPacket;
unsigned mp_hevc_nal_length_size(const uint8_t *extradata, size_t size);
bool mp_hevc_packet_has_dovi_rpu(const struct AVPacket *pkt, unsigned length_size);
bool mp_native_hdr10_packet(const struct AVCodecParameters *params, const struct AVPacket *pkt);
bool mp_render_context_get_recovery(mpv_render_context *ctx, uint64_t serial,
                                    bool *try_render);
