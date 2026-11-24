#ifndef MPV_CLIENT_API_RENDER_APPLE_H_
#define MPV_CLIENT_API_RENDER_APPLE_H_

#include <stdint.h>
#include <CoreGraphics/CoreGraphics.h>
#include "render.h"

/* Apple Native Render API v1, available since client API 2.9.
 * Future API extensions retain these layouts and their v1 requirements.
 */
#define MPV_APPLE_NATIVE_API_VERSION 1

typedef struct mpv_apple_native_init_params {
    uint32_t version; // Set to MPV_APPLE_NATIVE_API_VERSION.
    // AVSampleBufferDisplayLayer*, created and attached by the UI thread.
    // libmpv retains it until context destruction. mpv controls presentation
    // timing. Leave controlTimebase unset and keep the layer out of render synchronizers.
    void *display_layer;
} mpv_apple_native_init_params;

typedef struct mpv_apple_native_target {
    uint32_t version; // Set to MPV_APPLE_NATIVE_API_VERSION.
    int w, h; // Pixel dimensions, positive. Calls may run on a render thread.

    // Outputs in target pixels. Apply video_rect and contents_rect to the
    // layer on the UI thread. Apply osd to a separate, full-target overlay.
    // Set osd=NULL before each call; CGImageRelease any returned image, even
    // on failure. Layer geometry and these images must not modify video pixels.
    CGRect video_rect;
    CGRect contents_rect; // Normalized source crop.
    CGImageRef osd;
} mpv_apple_native_target;

/* Build libmpv with -Dapple-native=enabled. The native backend requires
 * macOS 14+ or iOS 17+. Check mpv_client_api_version() for major 2 and minor
 * >= 9, then check context creation. A native-only create() can return
 * MPV_ERROR_NOT_IMPLEMENTED when the backend is omitted from the build, or
 * MPV_ERROR_UNSUPPORTED when the system cannot initialize it. Successful
 * creation does not guarantee playback support for any particular stream.
 *
 * Use API_TYPE="apple-native", OUTPUT_MODE="native" and APPLE_NATIVE_INIT_PARAMS
 * for a native-only context. Native output uses direct VideoToolbox decoding
 * and the caller's display layer. Explicit native output uses the codecs that
 * FFmpeg can negotiate with the device's VideoToolbox decoder. Use a layer
 * attached to the application's native layer hierarchy and eligible for HDR
 * presentation. Keep video pixels on that layer. Render overlays separately
 * and apply layer changes on the UI
 * thread. The mode query reports the selected output path. The OS and the
 * layer/display configuration determine the actual presentation range.
 *
 * Use the update/render/report_swap/free lifecycle. For native frames, pass
 * APPLE_NATIVE_TARGET. render() waits for mpv's target time, then enqueues the
 * frame. BLOCK_FOR_TARGET_TIME must remain enabled for native presentation.
 * SKIP_RENDERING consumes a frame without enqueueing it. The caller must not
 * enqueue, flush or change the layer's timebase while its context is alive.
 * Use one context per layer, serialize the render API calls, and free the
 * context before removing the layer. GPU color and target color-space controls,
 * ICC profiles, interpolation, shaders and screenshots apply to render output.
 *
 * For OUTPUT_MODE="auto", enable ADVANCED_CONTROL. Supply the normal render
 * initialization and the native initialization in the same context. Auto can
 * use a native-only context if the stream meets the native requirements.
 * Auto prefers native single-layer Dolby Vision Profile 5 or 8.1 and verified
 * 10-bit HEVC Main 10 HDR10 with BT.2020 and PQ, subject to hardware decoding,
 * platform HDR eligibility and processing options. It uses the render backend
 * for other formats when that backend is suitable.
 *
 * For HDR10 on macOS, set DISPLAY_HDR10_CAPABLE=1 when the display that hosts
 * the video layer supports HDR10. On physical iOS devices the default is to
 * rely on system HDR playback eligibility. The simulator does not inherit
 * that default. Setting 0 prevents selection of native HDR10. Update the
 * value before presenting on a different display, or recreate the context.
 * The system must also allow HDR playback. This parameter does not affect
 * the existing Dolby Vision eligibility requirements.
 * HDR10 works with standard FFmpeg. Native Dolby Vision requires matching
 * FFmpeg VideoToolbox support. Profile 8.1 requires macOS 27+ or iOS 27+.
 *
 * After update(), query OUTPUT_MODE before acquiring a target. Present only
 * the selected output. Use APPLE_NATIVE_TARGET for "native" and the
 * application's normal render target for "render". In "recovering", keep
 * servicing update callbacks and client events without acquiring a target.
 * A configured OpenGL fallback still requires its original context to be
 * current as usual, including during native output. Keep configured graphics
 * resources alive until context destruction and use the usual target retirement
 * rules for the render backend. MPV_RENDER_PARAM_ICC_PROFILE configures the
 * render fallback even while native output is active.
 *
 * Auto can switch once to normal decoding after native failure. Before video
 * reaches the output chain, it can replay buffered input without seeking.
 * Later recovery restarts decoding at the current position when seeking is
 * possible. After rejection, auto keeps native disabled for that context.
 * An unsupported stream or failed recovery can end the video track. Check the
 * regular playback events in addition to render() errors.
 */

#endif
