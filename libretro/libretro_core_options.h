/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Core Options v2 Configuration
 * Copyright (C) 2024 Xenia Edge Contributors
 */

#ifndef LIBRETRO_CORE_OPTIONS_H
#define LIBRETRO_CORE_OPTIONS_H

#include "libretro.h"

#include <stdlib.h>
#include <string.h>

// Forward declaration
struct xenia_core_state;

// Core option keys ??? Graphics
#define XENIA_OPT_GRAPHICS_API          "xenia_graphics_api"
#define XENIA_OPT_DRAW_RESOLUTION_SCALE "xenia_draw_resolution_scale"
#define XENIA_OPT_ANISOTROPIC_FILTERING "xenia_anisotropic_filtering"
#define XENIA_OPT_ASYNC_SHADERS         "xenia_async_shader_compilation"
#define XENIA_OPT_READBACK_RESOLVE      "xenia_readback_resolve"
#define XENIA_OPT_STORE_SHADERS         "xenia_store_shaders"
#define XENIA_OPT_HALF_PIXEL_OFFSET     "xenia_half_pixel_offset"
#define XENIA_OPT_GPU_INVALID_FETCH     "xenia_gpu_allow_invalid_fetch_constants"
#define XENIA_OPT_FUZZY_ALPHA_EPSILON   "xenia_use_fuzzy_alpha_epsilon"
#define XENIA_OPT_DISPLAY_REFRESH       "xenia_display_refresh"
#define XENIA_OPT_UNLOCK_FRAMERATE      "xenia_unlock_framerate"
#define XENIA_OPT_WAIT_FOR_FRAME        "xenia_wait_for_frame"

// Core option keys ??? Video
#define XENIA_OPT_INTERNAL_DISPLAY_RES  "xenia_internal_display_resolution"
#define XENIA_OPT_WIDESCREEN            "xenia_widescreen"
#define XENIA_OPT_VIDEO_STANDARD        "xenia_video_standard"
#define XENIA_OPT_DISPLAY_GAMMA         "xenia_display_gamma"

// Core option keys ??? Audio
#define XENIA_OPT_AUDIO_ENABLED         "xenia_audio_enabled"
#define XENIA_OPT_MUTE                  "xenia_mute"
#define XENIA_OPT_AUDIO_CHANNELS        "xenia_audio_channels"
#define XENIA_OPT_XMA_DECODER           "xenia_xma_decoder"
#define XENIA_OPT_DEDICATED_XMA_THREAD  "xenia_dedicated_xma_thread"
#define XENIA_OPT_ENABLE_XMP            "xenia_enable_xmp"
#define XENIA_OPT_XMP_DEFAULT_VOLUME    "xenia_xmp_default_volume"

// Core option keys ??? Emulation
#define XENIA_OPT_TIME_SCALAR           "xenia_time_scalar"
#define XENIA_OPT_TITLE_UPDATES         "xenia_title_updates"
#define XENIA_OPT_APPLY_PATCHES         "xenia_apply_patches"
#define XENIA_OPT_LICENSE_MASK          "xenia_license_mask"
#define XENIA_OPT_USER_LANGUAGE         "xenia_user_language"
#define XENIA_OPT_USER_COUNTRY          "xenia_user_country"

// Core option keys ??? Compatibility
#define XENIA_OPT_PROTECT_ZERO          "xenia_protect_zero"
#define XENIA_OPT_CLEAR_MEMORY_PAGE     "xenia_clear_memory_page_state"
#define XENIA_OPT_OCCLUSION_QUERY       "xenia_occlusion_query"
#define XENIA_OPT_OCCLUSION_FULL        "xenia_occlusion_query_full_counters"
#define XENIA_OPT_OCCLUSION_FAKE_LOWER  "xenia_occlusion_query_fake_lower_threshold"
#define XENIA_OPT_OCCLUSION_FAKE_UPPER  "xenia_occlusion_query_fake_upper_threshold"
#define XENIA_OPT_RENDER_TARGET_PATH    "xenia_render_target_path"
#define XENIA_OPT_DEPTH_BIAS_SHADER     "xenia_depth_bias_shader_offset"
#define XENIA_OPT_SCALE_THRESHOLD       "xenia_draw_resolution_scale_threshold"
#define XENIA_OPT_INVALID_UPLOAD        "xenia_gpu_allow_invalid_upload_range"
#define XENIA_OPT_GAMMA_UNORM16         "xenia_gamma_render_target_as_unorm16"
#define XENIA_OPT_FORCE_DEPTH_CLAMP     "xenia_force_depth_clamp"
#define XENIA_OPT_IGNORE_RANGED_OFFSET  "xenia_ignore_offset_for_ranged_allocations"
#define XENIA_OPT_BREAK_UNIMPLEMENTED   "xenia_break_on_unimplemented_instructions"
#define XENIA_OPT_SCRIBBLE_HEAP         "xenia_scribble_heap"
#define XENIA_OPT_SCRIBBLE_HEAP_VALUE   "xenia_scribble_heap_value"
#define XENIA_OPT_DISABLE_CTX_PROMOTION "xenia_disable_context_promotion"
#define XENIA_OPT_MOUNT_CACHE           "xenia_mount_cache"
#define XENIA_OPT_MOUNT_SCRATCH         "xenia_mount_scratch"

// Core option keys ??? Debug
#define XENIA_OPT_LOG_LEVEL             "xenia_log_level"

// Core option value constants
#define XENIA_GRAPHICS_D3D12   "d3d12"
#define XENIA_GRAPHICS_VULKAN  "vulkan"

#define XENIA_LOG_LEVEL_ERROR  "error"
#define XENIA_LOG_LEVEL_WARN   "warn"
#define XENIA_LOG_LEVEL_INFO   "info"
#define XENIA_LOG_LEVEL_DEBUG  "debug"

/*
 * retro_core_option_v2_definition fields:
 *   key, desc, desc_categorized, info, info_categorized,
 *   category_key, values[RETRO_NUM_CORE_OPTION_VALUES_MAX], default_value
 */
static struct retro_core_option_v2_definition xenia_core_options_v2_defs[] = {
    /* ================================================================ */
    /* --- Graphics ---                                                  */
    /* ================================================================ */
    {
        XENIA_OPT_GRAPHICS_API,
        "Graphics API (Restart)",
        "Graphics API",
        "The GPU backend Xenia emulates the Xbox 360 GPU with, and the "
        "context the core asks RetroArch for: RetroArch changes its video "
        "driver to match when it is allowed to (Settings > Video > Output). "
        "Applies when the content is loaded again.",
        NULL,
        "Graphics",
        {
            { "vulkan", "Vulkan" },
#ifdef _WIN32
            { "d3d12",  "Direct3D 12" },
#endif
            { NULL, NULL }
        },
        "vulkan"
    },
    {
        XENIA_OPT_RENDER_TARGET_PATH,
        "Render Target Path",
        "RT Path",
        "Select render target emulation mode.\n"
        "Performance: host render targets with fixed-function blending.\n"
        "Accuracy: pixel shader interlock / rasterizer-ordered views.",
        NULL,
        "Graphics",
        {
            { "performance", "Performance" },
            { "accuracy",    "Accuracy" },
            { NULL, NULL }
        },
        "performance"
    },
    {
        XENIA_OPT_DRAW_RESOLUTION_SCALE,
        "Draw Resolution Scale (Restart)",
        "Resolution Scale",
        "Scale the internal rendering resolution. Higher values improve "
        "image quality but require more GPU power. Requires restart.",
        NULL,
        "Graphics",
        {
            { "1", "1x (Native 720p)" },
            { "2", "2x (1440p)" },
            { "3", "3x (2160p/4K)" },
            { "4", "4x" },
            { "5", "5x" },
            { "6", "6x" },
            { "7", "7x" },
            { "8", "8x" },
            { NULL, NULL }
        },
        "1"
    },
    {
        XENIA_OPT_ANISOTROPIC_FILTERING,
        "Anisotropic Filtering",
        "Anisotropic",
        "Override anisotropic filtering level for all textures. "
        "'Default' uses the game's own settings.",
        NULL,
        "Graphics",
        {
            { "-1", "Default (Game)" },
            { "0",  "Off" },
            { "1",  "2x" },
            { "2",  "4x" },
            { "3",  "8x" },
            { "4",  "16x" },
            { NULL, NULL }
        },
        "-1"
    },
    {
        XENIA_OPT_ASYNC_SHADERS,
        "Async Shader Compilation",
        "Async Shaders",
        "Compile shaders in background threads. Reduces stutter but may "
        "cause brief rendering artifacts.",
        NULL,
        "Graphics",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_READBACK_RESOLVE,
        "Readback Resolve",
        "Readback",
        "Copy render-to-texture output back into guest RAM when the CPU "
        "accesses it. Off breaks games that read it back.",
        NULL,
        "Graphics",
        {
            { "enabled", NULL },
            { "disabled", NULL },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_STORE_SHADERS,
        "Store Shaders",
        "Shader Cache",
        "Store compiled shaders persistently to avoid recompilation stutter "
        "on subsequent runs.",
        NULL,
        "Graphics",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_HALF_PIXEL_OFFSET,
        "Half-Pixel Offset",
        "Half-Pixel",
        "Enable D3D9-style half-pixel offset. Correct for most games. "
        "Disable if you see shifted/blurry rendering.",
        NULL,
        "Graphics",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_GPU_INVALID_FETCH,
        "Allow Invalid Fetch Constants",
        "Invalid Fetch",
        "Allow texture/vertex fetch constants with invalid type. May fix "
        "crashes in some games but is generally unsafe.",
        NULL,
        "Graphics",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_FUZZY_ALPHA_EPSILON,
        "Fuzzy Alpha Epsilon (NVIDIA Fix)",
        "Fuzzy Alpha",
        "Use approximate alpha comparison to prevent flickering on "
        "NVIDIA GPUs.",
        NULL,
        "Graphics",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_DISPLAY_REFRESH,
        "Emulated Display Refresh Rate",
        "Display Refresh Rate",
        "The refresh rate of the TV the game thinks it is on, as standalone's "
        "option. NTSC (60 Hz) is the console's. PAL (50 Hz) is a PAL TV: "
        "games run at 50 frames a second. Unlock Framerate overrides this.",
        NULL,
        "Graphics",
        {
            { "60", "NTSC (60Hz)" },
            { "50", "PAL (50Hz)" },
            { NULL, NULL }
        },
        "60"
    },
    {
        XENIA_OPT_WAIT_FOR_FRAME,
        "Wait for the Game's Frame",
        "Wait for the Game's Frame",
        "Removes 1 frame of input lag at the possible cost of the game slowing down.",
        NULL,
        "Graphics",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_UNLOCK_FRAMERATE,
        "Unlock Framerate",
        "Unlock Framerate",
        "Only for games whose logic is not tied to their frame rate, else "
        "they run too fast. 30 fps games at 60: two vblanks for each frame "
        "RetroArch shows. 60 fps games at 120: the core at 120 frames a "
        "second (set RetroArch and the display up for it). Uncapped: vblanks "
        "as fast as possible. Listed games: the rate "
        "system/Xenia-Edge/xenia_framerate_unlock.txt gives the running game, "
        "if it lists it. Can be changed while a game runs.",
        NULL,
        "Graphics",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Listed games" },
            { "30to60",   "30 fps games at 60" },
            { "120",      "60 fps games at 120" },
            { "uncapped", "Uncapped" },
            { NULL, NULL }
        },
        "disabled"
    },
    /* ================================================================ */
    /* --- Video ---                                                     */
    /* ================================================================ */
    {
        XENIA_OPT_INTERNAL_DISPLAY_RES,
        "Internal Display Resolution (Restart)",
        "Display Resolution",
        "Allow games that support multiple resolutions to render at a "
        "specific resolution. Not all games support this.",
        NULL,
        "Video",
        {
            { "0",  "640x480" },
            { "1",  "640x576" },
            { "2",  "720x480" },
            { "3",  "720x576" },
            { "4",  "800x600" },
            { "5",  "848x480" },
            { "6",  "1024x768" },
            { "7",  "1152x864" },
            { "8",  "1280x720 (Default)" },
            { "9",  "1280x768" },
            { "10", "1280x1024" },
            { "11", "1360x768" },
            { "12", "1440x900" },
            { "13", "1680x1050" },
            { "14", "1920x540" },
            { "15", "1920x1080" },
            { NULL, NULL }
        },
        "8"
    },
    {
        XENIA_OPT_WIDESCREEN,
        "Widescreen (16:9)",
        "Widescreen",
        "Toggle between 16:9 widescreen and 4:3 standard aspect ratio.",
        NULL,
        "Video",
        {
            { "enabled",  "16:9 Widescreen" },
            { "disabled", "4:3 Standard" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_VIDEO_STANDARD,
        "Video Standard",
        "Signal",
        "Select the video signal standard. Affects region detection.",
        NULL,
        "Video",
        {
            { "1", "NTSC" },
            { "2", "NTSC-J (Japan)" },
            { "3", "PAL" },
            { NULL, NULL }
        },
        "1"
    },
    {
        XENIA_OPT_DISPLAY_GAMMA,
        "Display Gamma",
        "Gamma",
        "Select display gamma curve.\n"
        "BT.709 (HDTV) is closest to Xbox 360 on a modern display.",
        NULL,
        "Video",
        {
            { "0", "Linear" },
            { "1", "sRGB (CRT)" },
            { "2", "BT.709 (HDTV)" },
            { NULL, NULL }
        },
        "2"
    },
    /* ================================================================ */
    /* --- Audio ---                                                     */
    /* ================================================================ */
    {
        XENIA_OPT_AUDIO_ENABLED,
        "Audio Output",
        NULL,
        "Enable or disable audio processing and output.",
        NULL,
        "Audio",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_MUTE,
        "Mute Audio",
        "Mute",
        "Mute all audio output while keeping audio processing active.",
        NULL,
        "Audio",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled (Muted)" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_AUDIO_CHANNELS,
        "Audio Channels (Restart)",
        "Audio Channels",
        "Stereo: the console's 5.1 folded down to two channels. Surround 5.1: all six channels to RetroArch, whose audio output layout then decides between speakers and a fold-down. Needs a RetroArch with multi-channel audio output, else stays stereo.",
        NULL,
        "Audio",
        {
            { "stereo", "Stereo" },
            { "5.1",    "Surround 5.1" },
            { NULL, NULL }
        },
        "stereo"
    },
    {
        XENIA_OPT_XMA_DECODER,
        "XMA Decoder (Restart)",
        "XMA Decoder",
        "Select the XMA audio decoder implementation. Try a different "
        "option if audio is broken in a specific game.",
        NULL,
        "Audio",
        {
            { "new",    "New (Default)" },
            { "old",    "Old" },
            { "master", "Master" },
            { "fake",   "Fake (Silence)" },
            { NULL, NULL }
        },
        "new"
    },
    {
        XENIA_OPT_DEDICATED_XMA_THREAD,
        "Dedicated XMA Thread",
        "XMA Thread",
        "Use a dedicated thread for XMA audio decoding. May improve "
        "audio performance.",
        NULL,
        "Audio",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_ENABLE_XMP,
        "Music Player (XMP)",
        "XMP",
        "Enable the Xbox Music Player for background music playback.",
        NULL,
        "Audio",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_XMP_DEFAULT_VOLUME,
        "Music Player Volume",
        "XMP Volume",
        "Default music volume if the game doesn't set it.",
        NULL,
        "Audio",
        {
            { "0",   "0%" },
            { "10",  "10%" },
            { "20",  "20%" },
            { "30",  "30%" },
            { "40",  "40%" },
            { "50",  "50%" },
            { "60",  "60%" },
            { "70",  "70%" },
            { "80",  "80%" },
            { "90",  "90%" },
            { "100", "100% (Default)" },
            { NULL, NULL }
        },
        "100"
    },
    /* ================================================================ */
    /* --- Emulation ---                                                 */
    /* ================================================================ */
    {
        XENIA_OPT_TIME_SCALAR,
        "Emulation Speed",
        "Speed",
        "Control emulation speed. 1.0x is normal speed.",
        NULL,
        "Emulation",
        {
            { "0.25", "0.25x" },
            { "0.5",  "0.5x" },
            { "1.0",  "1.0x (Normal)" },
            { "2.0",  "2.0x" },
            { "4.0",  "4.0x" },
            { NULL, NULL }
        },
        "1.0"
    },
    {
        XENIA_OPT_TITLE_UPDATES,
        "Apply Title Updates",
        "Title Updates",
        "Apply title update patches if available in the content directory.",
        NULL,
        "Emulation",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_APPLY_PATCHES,
        "Apply Game Patches",
        "Patches",
        "Enable custom game patching functionality.",
        NULL,
        "Emulation",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_LICENSE_MASK,
        "License Mask",
        "License",
        "Set license mask for activated content (DLC, full version).\n"
        "None: no licenses. Full: first license. All: all licenses.",
        NULL,
        "Emulation",
        {
            { "0",  "None" },
            { "1",  "Full" },
            { "-1", "All" },
            { NULL, NULL }
        },
        "0"
    },
    {
        XENIA_OPT_USER_LANGUAGE,
        "User Language",
        "Language",
        "Set the emulated console language.",
        NULL,
        "Emulation",
        {
            { "English",    "English" },
            { "Japanese",   "Japanese" },
            { "German",     "German" },
            { "French",     "French" },
            { "Spanish",    "Spanish" },
            { "Italian",    "Italian" },
            { "Korean",     "Korean" },
            { "TChinese",   "Traditional Chinese" },
            { "Portuguese", "Portuguese" },
            { "SChinese",   "Simplified Chinese" },
            { "Polish",     "Polish" },
            { "Russian",    "Russian" },
            { NULL, NULL }
        },
        "English"
    },
    {
        XENIA_OPT_USER_COUNTRY,
        "User Country",
        "Country",
        "Set the emulated console country/region.",
        NULL,
        "Emulation",
        {
            { "United States", "United States" },
            { "Great Britain", "Great Britain" },
            { "Japan",         "Japan" },
            { "Germany",       "Germany" },
            { "France",        "France" },
            { "Spain",         "Spain" },
            { "Italy",         "Italy" },
            { "Australia",     "Australia" },
            { "Canada",        "Canada" },
            { "Brazil",        "Brazil" },
            { "Korea",         "Korea" },
            { "China",         "China" },
            { "Mexico",        "Mexico" },
            { "Netherlands",   "Netherlands" },
            { "Russia",        "Russia" },
            { "Sweden",        "Sweden" },
            { "Poland",        "Poland" },
            { "Portugal",      "Portugal" },
            { "Taiwan",        "Taiwan" },
            { "Hong Kong",     "Hong Kong" },
            { NULL, NULL }
        },
        "United States"
    },
    /* ================================================================ */
    /* --- Compatibility ---                                             */
    /* ================================================================ */
    {
        XENIA_OPT_PROTECT_ZERO,
        "Protect Zero Page",
        "Protect Zero",
        "Protect the zero page from reads and writes. Disable if a game "
        "crashes on startup.",
        NULL,
        "Compatibility",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_CLEAR_MEMORY_PAGE,
        "Clear GPU Memory Page State",
        "Clear GPU Cache",
        "Refresh state of memory pages for GPU written data. Off by "
        "default, as on standalone; some games need it on to render right "
        "(Ridge Racer 6, and the others xenia-manager's optimized settings "
        "list).",
        NULL,
        "Compatibility",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_DISABLE_CTX_PROMOTION,
        "Disable Context Promotion",
        "No Ctx Promotion",
        "Disable context promotion CPU optimization. May be needed for "
        "some sports games, but reduces performance.",
        NULL,
        "Compatibility",
        {
            { "disabled", "Disabled (Normal)" },
            { "enabled",  "Enabled (Sports Fix)" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_MOUNT_CACHE,
        "Mount Cache Partition",
        "Cache Mount",
        "Enable cache partition mount. Required by some games.",
        NULL,
        "Compatibility",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_MOUNT_SCRATCH,
        "Mount Scratch Partition",
        "Scratch Mount",
        "Enable scratch partition mount. Required by some games.",
        NULL,
        "Compatibility",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_OCCLUSION_QUERY,
        "Occlusion Query",
        "Occlusion Query",
        "How occlusion queries (lens flares, culling, auto-exposure) are answered. Fast (default) asks the GPU without waiting; Fake writes a made-up result; Fast-alt keeps cached zero results; Strict waits for the GPU, most accurate.",
        NULL,
        "Compatibility",
        {
            { "fast", "Fast" },
            { "fake", "Fake" },
            { "fast-alt", "Fast-alt" },
            { "strict", "Strict" },
            { NULL, NULL }
        },
        "fast"
    },
    {
        XENIA_OPT_OCCLUSION_FULL,
        "Occlusion Query Full Counters",
        "Full Counters",
        "Emulate the ZFail, StencilFail and Total occlusion counters in shaders. Some games need them; costs performance.",
        NULL,
        "Compatibility",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_OCCLUSION_FAKE_LOWER,
        "Fake Occlusion Lower Threshold",
        "Fake Lower",
        "Lower end of the fake sample count written when occlusion queries are faked. -1 writes nothing (some games then wait forever).",
        NULL,
        "Compatibility",
        {
            { "80", "80" },
            { "-1", "-1" },
            { "0", "0" },
            { "1", "1" },
            { NULL, NULL }
        },
        "80"
    },
    {
        XENIA_OPT_OCCLUSION_FAKE_UPPER,
        "Fake Occlusion Upper Threshold",
        "Fake Upper",
        "Upper end of the fake sample count written when occlusion queries are faked.",
        NULL,
        "Compatibility",
        {
            { "100", "100" },
            { "0", "0" },
            { NULL, NULL }
        },
        "100"
    },
    {
        XENIA_OPT_DEPTH_BIAS_SHADER,
        "Depth Bias Through Shader",
        "Depth Bias Shader",
        "Route decal draws with polygon offset through the shader instead of host depth bias. Fixes z-fighting decals in some games.",
        NULL,
        "Compatibility",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_SCALE_THRESHOLD,
        "Resolution Scale Threshold",
        "Scale Threshold",
        "Render targets at or below this pitch in pixels are not upscaled by the resolution scale. Fixes some effects that break when scaled.",
        NULL,
        "Compatibility",
        {
            { "0", "0 (scale all)" },
            { "256", "256" },
            { "360", "360" },
            { "512", "512" },
            { NULL, NULL }
        },
        "0"
    },
    {
        XENIA_OPT_INVALID_UPLOAD,
        "Allow Invalid Upload Range",
        "Invalid Uploads",
        "Allow games to read data from pages marked as no access.",
        NULL,
        "Compatibility",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_GAMMA_UNORM16,
        "Gamma Render Target as UNORM16",
        "Gamma UNORM16",
        "Emulate gamma render targets with 16 bits per component where the host cannot do 8-bit piecewise gamma. Turn off if colors look wrong in a game.",
        NULL,
        "Compatibility",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_FORCE_DEPTH_CLAMP,
        "Force Depth Clamp",
        "Depth Clamp",
        "Use host depth clamping instead of near and far plane clipping.",
        NULL,
        "Compatibility",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_IGNORE_RANGED_OFFSET,
        "Ignore Offset for Ranged Allocations",
        "Ranged Allocations",
        "Ignore the 4 KB offset for physical allocations with a provided range. Needed by a few games.",
        NULL,
        "Compatibility",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_BREAK_UNIMPLEMENTED,
        "Break on Unimplemented Instructions",
        "Break Unimplemented",
        "Stop when a game runs a CPU instruction Xenia does not implement. In a core that ends RetroArch; turn off to let such games go on.",
        NULL,
        "Compatibility",
        {
            { "enabled", "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_SCRIBBLE_HEAP,
        "Scribble Heap",
        "Scribble Heap",
        "Fill newly allocated heap memory with a value, for games that rely on what is left there.",
        NULL,
        "Compatibility",
        {
            { "disabled", "Disabled" },
            { "enabled", "Enabled" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_SCRIBBLE_HEAP_VALUE,
        "Scribble Heap Value",
        "Scribble Value",
        "The value Scribble Heap fills memory with. 0 is random.",
        NULL,
        "Compatibility",
        {
            { "0", "0 (random)" },
            { "255", "255" },
            { NULL, NULL }
        },
        "0"
    },
    /* ================================================================ */
    /* --- Debug ---                                                     */
    /* ================================================================ */
    {
        XENIA_OPT_LOG_LEVEL,
        "Log Level",
        "Level",
        "Set the verbosity of logging output.",
        NULL,
        "Debug",
        {
            { XENIA_LOG_LEVEL_ERROR, "Error Only" },
            { XENIA_LOG_LEVEL_WARN,  "Warning" },
            { XENIA_LOG_LEVEL_INFO,  "Info" },
            { XENIA_LOG_LEVEL_DEBUG, "Debug" },
            { NULL, NULL }
        },
        XENIA_LOG_LEVEL_INFO
    },
    /* Terminator */
    { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL }
};

static struct retro_core_option_v2_category xenia_core_option_categories[] = {
    { "Graphics", "Graphics Settings",
      "GPU backend, resolution scale, render target path, filtering." },
    { "Video", "Video Settings",
      "Display resolution, aspect ratio, video standard, gamma." },
    { "Audio", "Audio Settings",
      "Audio output, XMA decoder, music player." },
    { "Emulation", "Emulation Settings",
      "Speed, language, country, licenses, patches." },
    { "Compatibility", "Compatibility Settings",
      "Per-game hacks, memory, cache/scratch mounts." },
    { "Debug", "Debug Settings",
      "Log verbosity." },
    { NULL, NULL, NULL }
};

static struct retro_core_options_v2 xenia_core_options_v2_def = {
    xenia_core_option_categories,
    xenia_core_options_v2_defs
};

#endif /* LIBRETRO_CORE_OPTIONS_H */
