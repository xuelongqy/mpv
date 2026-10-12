#ifndef MPV_CLIENT_API_RENDER_APPLE_H_
#define MPV_CLIENT_API_RENDER_APPLE_H_

#include <stdint.h>
#include <CoreGraphics/CoreGraphics.h>
#include "render.h"

/* Experimental, not installed or covered by client API 2.8. Use the header
 * from the exact candidate library. Existing OpenGL/Vulkan ABIs are unchanged.
 */
#define MPV_APPLE_NATIVE_API_VERSION 1

typedef struct mpv_apple_native_init_params {
    uint32_t version;
    // AVSampleBufferDisplayLayer*, created/attached by the UI thread. Retained
    // until context destruction. iOS 17+/macOS 14+. No controlTimebase or
    // AVSampleBufferRenderSynchronizer: mpv alone controls presentation timing.
    void *display_layer;
} mpv_apple_native_init_params;

typedef struct mpv_apple_native_target {
    uint32_t version;
    int w, h; // Pixel dimensions, positive. Calls may run on a render thread.

    // Outputs in target pixels. Apply video_rect and contents_rect to the
    // layer on the UI thread. Apply osd to a separate, full-target overlay.
    // Set osd=NULL before each call; CGImageRelease any returned image, even
    // on failure. Layer geometry and these images must not modify video pixels.
    CGRect video_rect;
    CGRect contents_rect; // Normalized source crop.
    CGImageRef osd;
} mpv_apple_native_target;

/* Create with API "apple-native" and OUTPUT_MODE="native", without RENDERER.
 * Use the existing update/render/report_swap/free lifecycle. render() prepares
 * the frame, waits for mpv's target time, then enqueues it for display.
 * BLOCK_FOR_TARGET_TIME=0 is unsupported; SKIP_RENDERING does not enqueue.
 * The caller must not enqueue, flush or change the layer's timebase while the
 * context is alive. Stop rendering and free the context before removing it.
 * A single context owns the layer; calls must be externally serialized.
 * native requests direct VideoToolbox decoding, not a copy/download path.
 * DV currently requires the separately validated FFmpeg Profile 5 patch.
 * GPU color controls, ICC, interpolation, shaders and screenshots are not
 * supported. Unsupported video formats fail; there is no software fallback.
 *
 * With OUTPUT_MODE="auto", provide the normal OpenGL/Vulkan initialization
 * and this native initialization in the same context. After update(), query
 * OUTPUT_MODE before acquiring any GPU target. Render native with this target,
 * or render with the normal GPU target; do not present a GPU image in native
 * mode. "recovering" means wait for the next update, without acquiring a target.
 * A configured OpenGL fallback still requires its original context to be
 * current for update/render/set_parameter/free, including during native output.
 * A native error may start one normal-decoder rebuild at the current PTS.
 * Thereafter native stays disabled for this context. Incompatible codecs,
 * filters, shaders, interpolation and explicit ICC use render directly.
 * Non-seekable native failures cannot be recovered and are reported as errors.
 */

#endif
