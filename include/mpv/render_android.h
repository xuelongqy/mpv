#ifndef MPV_CLIENT_API_RENDER_ANDROID_H_
#define MPV_CLIENT_API_RENDER_ANDROID_H_

#include <stdint.h>
#include "render.h"

/* Android Native Render API v1, available since client API 2.9.
 * Future API extensions retain these layouts and their v1 requirements.
 */
#define MPV_ANDROID_NATIVE_API_VERSION 1

typedef struct mpv_android_native_init_params {
    uint32_t version; // Set to MPV_ANDROID_NATIVE_API_VERSION.
    void *video_surface; // JNI reference to the Surface of a SurfaceView.
    void *osd_surface; // Optional JNI Surface reference for a separate overlay.
} mpv_android_native_init_params;

typedef struct mpv_android_native_target {
    uint32_t version; // Set to MPV_ANDROID_NATIVE_API_VERSION.
    int w, h; // Positive pixel dimensions of the full target area.
    int video_x, video_y, video_w, video_h; // Output video bounds in target pixels.
} mpv_android_native_target;

/* Supply API_TYPE="android-native" and ANDROID_NATIVE_INIT_PARAMS to create a
 * native-only context. For OUTPUT_MODE="auto", enable ADVANCED_CONTROL, pass
 * ANDROID_NATIVE_INIT_PARAMS alongside your ordinary graphics initialization,
 * and set DISPLAY_HDR10_CAPABLE=1 if the current video view's display reports
 * Display.HdrCapabilities.HDR_TYPE_HDR10. The default is 0. Auto prefers native
 * for qualifying 10-bit HEVC BT.2020 PQ with hardware decoding and otherwise
 * uses the render backend. The display and surface configuration determine
 * actual HDR presentation. Dolby Vision and HDR10+ use the render fallback.
 *
 * Before creating the context, register the application's JavaVM with the
 * same FFmpeg used by libmpv via av_jni_set_java_vm(). Pass live JNI references
 * at create(), and keep the Surfaces valid until context destruction. libmpv
 * retains the necessary references. Free the context before either Surface is
 * destroyed and create a new one when the Surfaces are available again. Use
 * three distinct Surfaces if supplying an overlay and a GPU fallback.
 *
 * Service render updates and query OUTPUT_MODE before acquiring a target. In
 * "native", pass ANDROID_NATIVE_TARGET with BLOCK_FOR_TARGET_TIME enabled.
 * Apply the returned video rectangle to the video view on the UI thread. In
 * "render", present your normal render target and hide both native views.
 * While "recovering", keep servicing update callbacks and playback events.
 * Keep graphics resources alive for the configured fallback, including a
 * current OpenGL context when required. Native video uses the video Surface's
 * MediaCodec producer. Do not draw to that Surface or let the OS replace it
 * while the render context is alive. SKIP_RENDERING consumes a frame without
 * presenting it. Native handles aspect and letterboxing. Rendering that needs
 * to crop, transform or process video pixels requires a render fallback.
 *
 * If present, the optional overlay must support premultiplied RGBA8888 and
 * match the full target dimensions. Place it above the video on the UI thread.
 * libmpv writes subtitles and OSD to this Surface without touching the video.
 *
 * Update DISPLAY_HDR10_CAPABLE before presenting on a different display, or
 * recreate the context with the new display's capability. Once native output
 * fails or loses display eligibility, auto keeps native disabled for that
 * context. If needed, it restarts decoding at the current position. Check the
 * playback events for recovery or terminal errors.
 */

#endif
