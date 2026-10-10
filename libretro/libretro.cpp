/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Copyright (C) 2024 Xenia Edge Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "libretro.h"
#include "libretro_vulkan.h"
#ifdef _WIN32
#include "libretro_d3d12.h"
#endif
#include "libretro_core_options.h"

// Xenia headers
#include "xenia/xbox.h"
#include "xenia/emulator.h"
#include "xenia/memory.h"
#include "xenia/base/clock.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#ifdef _WIN32
#include "xenia/base/main_win.h"
#endif
#include "xenia/apu/apu_flags.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"

// CVars declared in .cc files - DECLARE for direct assignment.
DECLARE_bool(use_50Hz_mode);
#ifdef _WIN32
DECLARE_path(d3d12_runtime_path);
#endif
#ifndef __ANDROID__
DECLARE_path(log_file);  // not on Android, where Xenia logs to logcat
#endif
DECLARE_bool(apply_title_update);
DECLARE_string(xma_decoder);
DECLARE_int32(log_level);

// New cvars for expanded core options
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);
DECLARE_bool(readback_resolve);
DECLARE_bool(store_shaders);
DECLARE_bool(half_pixel_offset);
DECLARE_bool(gpu_allow_invalid_fetch_constants);
DECLARE_bool(use_fuzzy_alpha_epsilon);
DECLARE_uint32(internal_display_resolution);
DECLARE_bool(widescreen);
DECLARE_int32(video_standard);
DECLARE_uint32(kernel_display_gamma_type);
DECLARE_bool(use_dedicated_xma_thread);
DECLARE_bool(enable_xmp);
DECLARE_int32(xmp_default_volume);
DECLARE_bool(apply_patches);
DECLARE_int32(license_mask);
DECLARE_int32(user_language);
DECLARE_int32(user_country);
DECLARE_bool(protect_zero);
DECLARE_bool(clear_memory_page_state);
DECLARE_string(occlusion_query);
DECLARE_bool(occlusion_query_full_counters);
DECLARE_int32(occlusion_query_fake_lower_threshold);
DECLARE_int32(occlusion_query_fake_upper_threshold);
DECLARE_string(render_target_path);
DECLARE_bool(depth_bias_shader_offset);
DECLARE_uint32(draw_resolution_scale_threshold);
DECLARE_bool(gpu_allow_invalid_upload_range);
DECLARE_bool(gamma_render_target_as_unorm16);
DECLARE_bool(force_depth_clamp);
DECLARE_bool(ignore_offset_for_ranged_allocations);
DECLARE_bool(break_on_unimplemented_instructions);
DECLARE_bool(scribble_heap);
DECLARE_int32(scribble_heap_value);
DECLARE_bool(disable_context_promotion);
#ifdef _WIN32
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#endif
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/ui/presenter.h"
#include "libretro_vk_presenter.h"
#ifdef _WIN32
#include "libretro_d3d12_presenter.h"
#endif
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_state.h"
#include "libretro_audio_driver.h"
#include "libretro_hid.h"
#include "libretro_gamedb.h"
#include "libretro_disc.h"

// The Vulkan functions this file calls on RetroArch's device, from RetroArch's
// own loader through the HW render interface: the core links no Vulkan loader
// (Windows has none to link), and Xenia's own device has its own functions.
static struct {
    PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers = nullptr;
    PFN_vkAllocateMemory p_vkAllocateMemory = nullptr;
    PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer = nullptr;
    PFN_vkBindBufferMemory p_vkBindBufferMemory = nullptr;
    PFN_vkBindImageMemory p_vkBindImageMemory = nullptr;
    PFN_vkCmdCopyBufferToImage p_vkCmdCopyBufferToImage = nullptr;
    PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier = nullptr;
    PFN_vkCreateBuffer p_vkCreateBuffer = nullptr;
    PFN_vkCreateCommandPool p_vkCreateCommandPool = nullptr;
    PFN_vkCreateImage p_vkCreateImage = nullptr;
    PFN_vkCreateImageView p_vkCreateImageView = nullptr;
    PFN_vkDestroyBuffer p_vkDestroyBuffer = nullptr;
    PFN_vkDestroyCommandPool p_vkDestroyCommandPool = nullptr;
    PFN_vkDestroyImage p_vkDestroyImage = nullptr;
    PFN_vkDestroyImageView p_vkDestroyImageView = nullptr;
    PFN_vkDeviceWaitIdle p_vkDeviceWaitIdle = nullptr;
    PFN_vkEndCommandBuffer p_vkEndCommandBuffer = nullptr;
    PFN_vkFreeMemory p_vkFreeMemory = nullptr;
    PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements = nullptr;
    PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkMapMemory p_vkMapMemory = nullptr;
    PFN_vkQueueSubmit p_vkQueueSubmit = nullptr;
    PFN_vkResetCommandBuffer p_vkResetCommandBuffer = nullptr;
    PFN_vkUnmapMemory p_vkUnmapMemory = nullptr;
} lr_vk;
#define vkAllocateCommandBuffers lr_vk.p_vkAllocateCommandBuffers
#define vkAllocateMemory lr_vk.p_vkAllocateMemory
#define vkBeginCommandBuffer lr_vk.p_vkBeginCommandBuffer
#define vkBindBufferMemory lr_vk.p_vkBindBufferMemory
#define vkBindImageMemory lr_vk.p_vkBindImageMemory
#define vkCmdCopyBufferToImage lr_vk.p_vkCmdCopyBufferToImage
#define vkCmdPipelineBarrier lr_vk.p_vkCmdPipelineBarrier
#define vkCreateBuffer lr_vk.p_vkCreateBuffer
#define vkCreateCommandPool lr_vk.p_vkCreateCommandPool
#define vkCreateImage lr_vk.p_vkCreateImage
#define vkCreateImageView lr_vk.p_vkCreateImageView
#define vkDestroyBuffer lr_vk.p_vkDestroyBuffer
#define vkDestroyCommandPool lr_vk.p_vkDestroyCommandPool
#define vkDestroyImage lr_vk.p_vkDestroyImage
#define vkDestroyImageView lr_vk.p_vkDestroyImageView
#define vkDeviceWaitIdle lr_vk.p_vkDeviceWaitIdle
#define vkEndCommandBuffer lr_vk.p_vkEndCommandBuffer
#define vkFreeMemory lr_vk.p_vkFreeMemory
#define vkGetBufferMemoryRequirements lr_vk.p_vkGetBufferMemoryRequirements
#define vkGetImageMemoryRequirements lr_vk.p_vkGetImageMemoryRequirements
#define vkGetPhysicalDeviceMemoryProperties lr_vk.p_vkGetPhysicalDeviceMemoryProperties
#define vkMapMemory lr_vk.p_vkMapMemory
#define vkQueueSubmit lr_vk.p_vkQueueSubmit
#define vkResetCommandBuffer lr_vk.p_vkResetCommandBuffer
#define vkUnmapMemory lr_vk.p_vkUnmapMemory

// CVars from xenia_main.cc - libretro core replaces main entry point. apu and
// gpu are the emulator's own now (emulator.cc); the core sets gpu itself.
DECLARE_string(apu);
DECLARE_string(gpu);
DEFINE_string(hid, "nop", "Input system.", "HID");

DEFINE_path(storage_root, "", "Root path for persistent internal data storage.",
            "Storage");
DEFINE_path(content_root, "", "Root path for guest content storage.",
            "Storage");
DEFINE_path(cache_root, "", "Root path for cache files.", "Storage");

DECLARE_bool(mount_scratch);
DECLARE_bool(mount_cache);


DEFINE_transient_bool(portable, false, "Portable mode.", "General");
DEFINE_bool(discord, false, "Enable Discord rich presence", "General");

/* ================================================================== */
/*  Core state                                                         */
/* ================================================================== */
struct xenia_core_state {
    retro_video_refresh_t      video_cb;
    retro_audio_sample_t       audio_cb;
    retro_audio_sample_batch_t audio_batch_cb;
    retro_input_poll_t         input_poll_cb;
    retro_input_state_t        input_state_cb;
    retro_environment_t        environ_cb;

    int16_t *audio_buffer;
    size_t   audio_buffer_size;
    double   audio_sample_rate;

    char game_path[4096];
    char system_dir[4096];
    char save_dir[4096];

    char graphics_backend[32];
    bool vsync_enabled;
    int vblanks_per_run;
    bool fps120;
    // Wait for the Game's Frame
    bool wait_for_frame;
    // What RetroArch is told the core runs at (retro_get_system_av_info)
    double output_fps;
    bool audio_enabled;
    bool pal_mode;
    // The running game's title ID, read from its file before it launches
    // (0 when not known): the unlock list and the patches are matched by it
    uint32_t title_id;

    retro_log_printf_t log_cb;
};

static struct xenia_core_state core_state;
static std::unique_ptr<xe::Emulator> xenia_emulator;
static xe::apu::libretro::LibretroAudioSystem *audio_mixer = nullptr;

// RetroArch's multi-channel output, for 5.1 (NNshi). Once the frontend gives
// it, all of the game's audio goes through it.
#ifndef RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_MULTI
#define RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_MULTI (94 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_AUDIO_SPEAKER_FRONT_LEFT    0x001
#define RETRO_AUDIO_SPEAKER_FRONT_RIGHT   0x002
#define RETRO_AUDIO_SPEAKER_FRONT_CENTER  0x004
#define RETRO_AUDIO_SPEAKER_LOW_FREQUENCY 0x008
#define RETRO_AUDIO_SPEAKER_SIDE_LEFT     0x200
#define RETRO_AUDIO_SPEAKER_SIDE_RIGHT    0x400
typedef size_t (RETRO_CALLCONV *retro_audio_sample_batch_multi_int16_t)(
      const int16_t *data, size_t frames, unsigned channels, unsigned layout);
typedef size_t (RETRO_CALLCONV *retro_audio_sample_batch_multi_float_t)(
      const float *data, size_t frames, unsigned channels, unsigned layout);
struct retro_audio_sample_multi_callback
{
   retro_audio_sample_batch_multi_int16_t batch_int16;
   retro_audio_sample_batch_multi_float_t batch_float;
};
#endif
static retro_audio_sample_multi_callback s_audio_multi = {};
static unsigned s_audio_channels = 2;
static xe::hid::libretro_hid::LibretroInputDriver *lr_input_driver = nullptr;
static xe::gpu::GraphicsSystem *lr_graphics = nullptr;
static bool game_loaded = false;

// Software frame capture buffer (fallback path)
static xe::ui::RawImage captured_frame;

/* ================================================================== */
/*  Vulkan HW render state                                             */
/* ================================================================== */
#define VK_MAX_SYNC 8

// Frontend resources (frontend VkDevice from retro_hw_render_interface_vulkan)
struct VulkanFrameResources {
    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView image_view;
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    void *staging_mapped;
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd;
    uint32_t width;
    uint32_t height;
};

static const struct retro_hw_render_interface_vulkan *vk_hw = nullptr;
static struct retro_hw_render_callback hw_render_cb;
static VulkanFrameResources vk_frames[VK_MAX_SYNC] = {};
static struct retro_vulkan_image vk_current_image = {};
static bool vulkan_hw_render_active = false;

// The emulator starts once RetroArch has its Vulkan context: with the device
// negotiated (libretro_vk_create_shared_device), Xenia renders on the device
// RetroArch makes in its context setup, which comes after retro_load_game.
static bool s_launch_on_context_reset = false;
// Defined among the libretro API functions, inside their extern "C"
extern "C" {
static void launch_game(void);
}
static void xenia_stop_emulator(void);
static void xenia_log(enum retro_log_level level, const char *fmt, ...);

static const VkApplicationInfo *xenia_vk_application_info(void) {
    static const VkApplicationInfo info = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Xenia Edge", 0,
        "Xenia Edge", 0, VK_MAKE_API_VERSION(0, 1, 3, 0)};
    return &info;
}

static bool xenia_vk_create_device(
    struct retro_vulkan_context *context, VkInstance instance,
    VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const char **required_device_extensions,
    unsigned num_required_device_extensions,
    const char **required_device_layers, unsigned num_required_device_layers,
    const VkPhysicalDeviceFeatures *required_features) {
    VkPhysicalDevice made_gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    if (!libretro_vk_create_shared_device(
            instance, gpu, surface, get_instance_proc_addr,
            required_device_extensions, num_required_device_extensions,
            required_features, made_gpu, device, queue, family)) {
        xenia_log(RETRO_LOG_WARN,
                  "Vulkan: no device shared with RetroArch, frames go through host memory\n");
        return false;
    }
    context->gpu = made_gpu;
    context->device = device;
    context->queue = queue;
    context->queue_family_index = family;
    context->presentation_queue = queue;
    context->presentation_queue_family_index = family;
    return true;
}

static void xenia_vk_destroy_device(void) {
    libretro_vk_destroy_shared_device();
}

#ifdef _WIN32
/* ================================================================== */
/*  D3D12 HW render state                                              */
/* ================================================================== */
struct D3D12FrameResources {
    ID3D12Resource *texture;
    ID3D12Resource *upload_buffer;
    void *upload_mapped;
    ID3D12CommandAllocator *cmd_alloc;
    ID3D12GraphicsCommandList *cmd_list;
    ID3D12Fence *fence;
    HANDLE fence_event;
    UINT64 fence_value;
    uint32_t width;
    uint32_t height;
};

static const struct retro_hw_render_interface_d3d12 *d3d12_hw = nullptr;
static constexpr int D3D12_NUM_FRAMES = 2;
static D3D12FrameResources d3d12_frames[D3D12_NUM_FRAMES] = {};
static int d3d12_frame_idx = 0;
static bool d3d12_hw_render_active = false;
#else
static bool d3d12_hw_render_active = false;
#endif

/* ================================================================== */
/*  Logging                                                            */
/* ================================================================== */
static void xenia_log(enum retro_log_level level, const char *fmt, ...) {
    if (!core_state.log_cb) return;
    va_list va;
    va_start(va, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    core_state.log_cb(level, "[Xenia] %s", buf);
}

// Xenia's errors and warnings into the frontend's log, so that the one log a
// tester sends has them; the whole log goes to xenia.log in the save
// directory. The logger writes a line in pieces - its "!> f:... " prefix, the
// text, a newline - from its one writer thread, so they are put together here.
class RetroLogSink final : public xe::LogSink {
public:
    void Write(const char *buf, size_t size) override {
        line_.append(buf, size);
        size_t end;
        while ((end = line_.find('\n')) != std::string::npos) {
            const std::string line = line_.substr(0, end);
            line_.erase(0, end + 1);
            if (line.size() > 1 && line[1] == '>' && (line[0] == '!' || line[0] == 'w'))
                xenia_log(line[0] == '!' ? RETRO_LOG_ERROR : RETRO_LOG_WARN, "%s\n", line.c_str() + 3);
        }
    }
    void Flush() override {}

private:
    std::string line_;
};

/* ================================================================== */
/*  Vulkan HW render helpers                                           */
/* ================================================================== */
static uint32_t vk_find_memory_type(VkPhysicalDevice gpu,
                                     uint32_t type_bits,
                                     VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

/* ------------------------------------------------------------------ */
/*  Frontend-side Vulkan frame resources                               */
/* ------------------------------------------------------------------ */
static void vk_destroy_frame(VulkanFrameResources &f) {
    if (!vk_hw) return;
    VkDevice dev = vk_hw->device;
    if (f.image_view)      { vkDestroyImageView(dev, f.image_view, nullptr);   f.image_view = VK_NULL_HANDLE; }
    if (f.image)           { vkDestroyImage(dev, f.image, nullptr);            f.image = VK_NULL_HANDLE; }
    if (f.image_memory)    { vkFreeMemory(dev, f.image_memory, nullptr);       f.image_memory = VK_NULL_HANDLE; }
    if (f.staging_mapped && f.staging_memory) {
        vkUnmapMemory(dev, f.staging_memory);
        f.staging_mapped = nullptr;
    }
    if (f.staging_buffer)  { vkDestroyBuffer(dev, f.staging_buffer, nullptr);  f.staging_buffer = VK_NULL_HANDLE; }
    if (f.staging_memory)  { vkFreeMemory(dev, f.staging_memory, nullptr);     f.staging_memory = VK_NULL_HANDLE; }
    if (f.cmd_pool)        { vkDestroyCommandPool(dev, f.cmd_pool, nullptr);   f.cmd_pool = VK_NULL_HANDLE; }
    f.cmd = VK_NULL_HANDLE;
    f.width = f.height = 0;
}

static bool vk_create_frame(VulkanFrameResources &f,
                             uint32_t w, uint32_t h) {
    VkDevice dev = vk_hw->device;
    VkResult res;

    // Command pool
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk_hw->queue_index;
    res = vkCreateCommandPool(dev, &pool_info, nullptr, &f.cmd_pool);
    if (res != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = f.cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    res = vkAllocateCommandBuffers(dev, &alloc_info, &f.cmd);
    if (res != VK_SUCCESS) return false;

    // Image: TILING_OPTIMAL, SAMPLED|TRANSFER_DST|TRANSFER_SRC, MUTABLE_FORMAT
    VkImageCreateInfo img_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent = {w, h, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    res = vkCreateImage(dev, &img_info, nullptr, &f.image);
    if (res != VK_SUCCESS) return false;

    VkMemoryRequirements img_reqs;
    vkGetImageMemoryRequirements(dev, f.image, &img_reqs);
    VkMemoryAllocateInfo img_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    img_alloc.allocationSize = img_reqs.size;
    img_alloc.memoryTypeIndex = vk_find_memory_type(
        vk_hw->gpu, img_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_alloc.memoryTypeIndex == UINT32_MAX) return false;
    res = vkAllocateMemory(dev, &img_alloc, nullptr, &f.image_memory);
    if (res != VK_SUCCESS) return false;
    vkBindImageMemory(dev, f.image, f.image_memory, 0);

    // Image view
    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = f.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY};
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    res = vkCreateImageView(dev, &view_info, nullptr, &f.image_view);
    if (res != VK_SUCCESS) return false;

    // Staging buffer: host-visible, persistently mapped
    VkDeviceSize buf_size = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo buf_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf_info.size = buf_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    res = vkCreateBuffer(dev, &buf_info, nullptr, &f.staging_buffer);
    if (res != VK_SUCCESS) return false;

    VkMemoryRequirements buf_reqs;
    vkGetBufferMemoryRequirements(dev, f.staging_buffer, &buf_reqs);
    VkMemoryAllocateInfo buf_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    buf_alloc.allocationSize = buf_reqs.size;
    buf_alloc.memoryTypeIndex = vk_find_memory_type(
        vk_hw->gpu, buf_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_alloc.memoryTypeIndex == UINT32_MAX) return false;
    res = vkAllocateMemory(dev, &buf_alloc, nullptr, &f.staging_memory);
    if (res != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, f.staging_buffer, f.staging_memory, 0);
    vkMapMemory(dev, f.staging_memory, 0, VK_WHOLE_SIZE, 0, &f.staging_mapped);

    f.width = w;
    f.height = h;
    return true;
}

static void vk_destroy_all_frames(void) {
    for (int i = 0; i < VK_MAX_SYNC; i++)
        vk_destroy_frame(vk_frames[i]);
}

static void vulkan_context_reset(void) {
    const struct retro_hw_render_interface *iface = nullptr;
    if (!core_state.environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) ||
        !iface ||
        iface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
        iface->interface_version < RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION) {
        xenia_log(RETRO_LOG_ERROR,
                  "Failed to get Vulkan HW render interface (type=%d ver=%u)\n",
                  iface ? iface->interface_type : -1,
                  iface ? iface->interface_version : 0);
        vulkan_hw_render_active = false;
        return;
    }
    vk_hw = (const struct retro_hw_render_interface_vulkan *)iface;
    vulkan_hw_render_active = true;
    // One queue for both: Xenia takes RetroArch's lock around its
    // submissions to it
    libretro_vk_set_shared_queue_lock(
        [](void *) { vk_hw->lock_queue(vk_hw->handle); },
        [](void *) { vk_hw->unlock_queue(vk_hw->handle); }, nullptr);
    lr_vk.p_vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)vk_hw->get_device_proc_addr(vk_hw->device, "vkAllocateCommandBuffers");
    lr_vk.p_vkAllocateMemory = (PFN_vkAllocateMemory)vk_hw->get_device_proc_addr(vk_hw->device, "vkAllocateMemory");
    lr_vk.p_vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)vk_hw->get_device_proc_addr(vk_hw->device, "vkBeginCommandBuffer");
    lr_vk.p_vkBindBufferMemory = (PFN_vkBindBufferMemory)vk_hw->get_device_proc_addr(vk_hw->device, "vkBindBufferMemory");
    lr_vk.p_vkBindImageMemory = (PFN_vkBindImageMemory)vk_hw->get_device_proc_addr(vk_hw->device, "vkBindImageMemory");
    lr_vk.p_vkCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)vk_hw->get_device_proc_addr(vk_hw->device, "vkCmdCopyBufferToImage");
    lr_vk.p_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)vk_hw->get_device_proc_addr(vk_hw->device, "vkCmdPipelineBarrier");
    lr_vk.p_vkCreateBuffer = (PFN_vkCreateBuffer)vk_hw->get_device_proc_addr(vk_hw->device, "vkCreateBuffer");
    lr_vk.p_vkCreateCommandPool = (PFN_vkCreateCommandPool)vk_hw->get_device_proc_addr(vk_hw->device, "vkCreateCommandPool");
    lr_vk.p_vkCreateImage = (PFN_vkCreateImage)vk_hw->get_device_proc_addr(vk_hw->device, "vkCreateImage");
    lr_vk.p_vkCreateImageView = (PFN_vkCreateImageView)vk_hw->get_device_proc_addr(vk_hw->device, "vkCreateImageView");
    lr_vk.p_vkDestroyBuffer = (PFN_vkDestroyBuffer)vk_hw->get_device_proc_addr(vk_hw->device, "vkDestroyBuffer");
    lr_vk.p_vkDestroyCommandPool = (PFN_vkDestroyCommandPool)vk_hw->get_device_proc_addr(vk_hw->device, "vkDestroyCommandPool");
    lr_vk.p_vkDestroyImage = (PFN_vkDestroyImage)vk_hw->get_device_proc_addr(vk_hw->device, "vkDestroyImage");
    lr_vk.p_vkDestroyImageView = (PFN_vkDestroyImageView)vk_hw->get_device_proc_addr(vk_hw->device, "vkDestroyImageView");
    lr_vk.p_vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)vk_hw->get_device_proc_addr(vk_hw->device, "vkDeviceWaitIdle");
    lr_vk.p_vkEndCommandBuffer = (PFN_vkEndCommandBuffer)vk_hw->get_device_proc_addr(vk_hw->device, "vkEndCommandBuffer");
    lr_vk.p_vkFreeMemory = (PFN_vkFreeMemory)vk_hw->get_device_proc_addr(vk_hw->device, "vkFreeMemory");
    lr_vk.p_vkGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)vk_hw->get_device_proc_addr(vk_hw->device, "vkGetBufferMemoryRequirements");
    lr_vk.p_vkGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)vk_hw->get_device_proc_addr(vk_hw->device, "vkGetImageMemoryRequirements");
    lr_vk.p_vkMapMemory = (PFN_vkMapMemory)vk_hw->get_device_proc_addr(vk_hw->device, "vkMapMemory");
    lr_vk.p_vkQueueSubmit = (PFN_vkQueueSubmit)vk_hw->get_device_proc_addr(vk_hw->device, "vkQueueSubmit");
    lr_vk.p_vkResetCommandBuffer = (PFN_vkResetCommandBuffer)vk_hw->get_device_proc_addr(vk_hw->device, "vkResetCommandBuffer");
    lr_vk.p_vkUnmapMemory = (PFN_vkUnmapMemory)vk_hw->get_device_proc_addr(vk_hw->device, "vkUnmapMemory");
    lr_vk.p_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)vk_hw->get_instance_proc_addr(vk_hw->instance, "vkGetPhysicalDeviceMemoryProperties");
    xenia_log(RETRO_LOG_INFO,
              "Vulkan HW render interface acquired (ver %u, device %p, %s)\n",
              vk_hw->interface_version, (void *)vk_hw->device,
              libretro_vk_shared_device_active() ? "shared with Xenia"
                                                 : "RetroArch's own");
    if (s_launch_on_context_reset) {
        s_launch_on_context_reset = false;
        launch_game();
    }
}

static void vulkan_context_destroy(void) {
    // The device goes with the context (a video reinit while a game runs):
    // the emulator on it stops first and starts again with the new one
    if (libretro_vk_shared_device_active() && game_loaded) {
        xenia_log(RETRO_LOG_WARN, "Vulkan context destroyed while the game runs: restarting it on the new one\n");
        xenia_stop_emulator();
        s_launch_on_context_reset = true;
    }
    if (vk_hw) {
        vkDeviceWaitIdle(vk_hw->device);
        vk_destroy_all_frames();
    }
    vk_hw = nullptr;
    vulkan_hw_render_active = false;
    memset(&vk_current_image, 0, sizeof(vk_current_image));
}

#ifdef _WIN32
/* ================================================================== */
/*  D3D12 HW render helpers                                            */
/* ================================================================== */
static void d3d12_destroy_frame(D3D12FrameResources &f) {
    if (f.upload_mapped && f.upload_buffer) {
        f.upload_buffer->Unmap(0, nullptr);
        f.upload_mapped = nullptr;
    }
    if (f.cmd_list)   { f.cmd_list->Release();   f.cmd_list = nullptr; }
    if (f.cmd_alloc)  { f.cmd_alloc->Release();  f.cmd_alloc = nullptr; }
    if (f.fence)      { f.fence->Release();      f.fence = nullptr; }
    if (f.fence_event){ CloseHandle(f.fence_event); f.fence_event = nullptr; }
    if (f.upload_buffer) { f.upload_buffer->Release(); f.upload_buffer = nullptr; }
    if (f.texture)    { f.texture->Release();    f.texture = nullptr; }
    f.fence_value = 0;
    f.width = f.height = 0;
}

static bool d3d12_create_frame(D3D12FrameResources &f,
                                ID3D12Device *device,
                                uint32_t w, uint32_t h) {
    HRESULT hr;

    // Command allocator + list
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&f.cmd_alloc));
    if (FAILED(hr)) return false;

    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        f.cmd_alloc, nullptr,
        IID_PPV_ARGS(&f.cmd_list));
    if (FAILED(hr)) return false;
    f.cmd_list->Close();

    // Fence
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence));
    if (FAILED(hr)) return false;
    f.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    f.fence_value = 0;

    // Texture: DEFAULT heap, COPY_DEST ??? COPY_SOURCE
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = w;
    tex_desc.Height = h;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE,
        &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&f.texture));
    if (FAILED(hr)) return false;

    // Upload buffer (row-pitch aligned)
    UINT64 upload_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &layout, nullptr, nullptr, &upload_size);

    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = upload_size;
    buf_desc.Height = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE,
        &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&f.upload_buffer));
    if (FAILED(hr)) return false;

    // Persistently map
    D3D12_RANGE read_range = {0, 0};
    hr = f.upload_buffer->Map(0, &read_range, &f.upload_mapped);
    if (FAILED(hr)) return false;

    f.width = w;
    f.height = h;
    return true;
}

static void d3d12_context_reset(void) {
    const struct retro_hw_render_interface *iface = nullptr;
    if (!core_state.environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) ||
        !iface ||
        iface->interface_type != RETRO_HW_RENDER_INTERFACE_D3D12 ||
        iface->interface_version < RETRO_HW_RENDER_INTERFACE_D3D12_VERSION) {
        xenia_log(RETRO_LOG_ERROR,
                  "Failed to get D3D12 HW render interface (type=%d ver=%u)\n",
                  iface ? iface->interface_type : -1,
                  iface ? iface->interface_version : 0);
        d3d12_hw_render_active = false;
        return;
    }
    d3d12_hw = (const struct retro_hw_render_interface_d3d12 *)iface;
    d3d12_hw_render_active = true;
    xenia_log(RETRO_LOG_INFO,
              "D3D12 HW render interface acquired (ver %u, device %p)\n",
              d3d12_hw->interface_version, (void *)d3d12_hw->device);
}

static void d3d12_destroy_all_frames(void) {
    for (int i = 0; i < D3D12_NUM_FRAMES; i++)
        d3d12_destroy_frame(d3d12_frames[i]);
    d3d12_frame_idx = 0;
}

static void d3d12_context_destroy(void) {
    d3d12_destroy_all_frames();
    d3d12_hw = nullptr;
    d3d12_hw_render_active = false;
}
#endif /* _WIN32 */

/* ================================================================== */
/*  Core options helper (operates on xenia_core_state)                 */
/* ================================================================== */
static const char *opt_get(const char *key) {
    struct retro_variable var = {key, nullptr};
    if (core_state.environ_cb &&
        core_state.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return var.value;
    return nullptr;
}

// A message box the core answered without a window (xam_ui.cc), from a guest
// thread, shown by retro_run on RetroArch's screen
static std::mutex s_notice_mutex;
static std::string s_notice;

// system/Xenia-Edge, where the core keeps everything (xenia_setup_and_launch)
static std::filesystem::path xenia_root_dir(void) {
    return std::filesystem::path(core_state.system_dir) / "Xenia-Edge";
}

// No D3D12 device on this Windows machine: the Graphics API option is hidden
// (retro_set_environment), at every publish
static bool s_hide_graphics_api = false;

// The core options, and with a game whose patches the core has the Patch
// Manager category after them: one option per patch, published again once
// the game's module is loaded (and without them after it is unloaded).
// RetroArch keeps each option's value in the core's options file.
static void publish_core_options(void) {
    static std::vector<retro_core_option_v2_definition> defs;
    static std::vector<retro_core_option_v2_category> cats;
    static retro_core_options_v2 options;
    defs.clear();
    cats.clear();
    for (const auto *d = xenia_core_options_v2_defs; d->key; ++d)
        defs.push_back(*d);
    for (const auto *c = xenia_core_option_categories; c->key; ++c)
        cats.push_back(*c);
    const auto &patches = gamedb::patches();
    if (!patches.empty()) {
        cats.push_back({ "patches", "Patch Manager",
            "The game's patches from Xenia Edge's game-patches, for this "
            "version of it. A change applies at the next boot." });
        for (const auto &p : patches) {
            retro_core_option_v2_definition d{};
            d.key = p.key.c_str();
            d.desc = p.label.c_str();
            d.desc_categorized = p.label.c_str();
            d.info = p.info.empty() ? nullptr : p.info.c_str();
            d.category_key = "patches";
            d.values[0] = { "disabled", "Disabled" };
            d.values[1] = { "enabled", "Enabled" };
            d.values[2] = { nullptr, nullptr };
            d.default_value = p.default_on ? "enabled" : "disabled";
            defs.push_back(d);
        }
    }
    defs.push_back({});
    cats.push_back({ nullptr, nullptr, nullptr });
    options.categories = cats.data();
    options.definitions = defs.data();
    core_state.environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
    if (s_hide_graphics_api) {
        struct retro_core_option_display display = {
            XENIA_OPT_GRAPHICS_API, false};
        core_state.environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
    }
}

// Each patch file of the game, as bundled, with is_enabled from its option,
// into system/Xenia-Edge/patches, where Xenia reads them at boot
static void write_patch_files(void) {
    gamedb::write_patch_files(xenia_root_dir() / "patches",
        [](const gamedb::PatchOption &p) {
            const char *v = opt_get(p.key.c_str());
            return v ? strcmp(v, "enabled") == 0 : p.default_on;
        });
}

static void apply_core_options(void) {
    const char *v;

    // =================================================================
    // Graphics
    // =================================================================

    // Render target path
    if ((v = opt_get(XENIA_OPT_RENDER_TARGET_PATH))) {
        cvars::render_target_path = v;
    }

    // Draw resolution scale (uniform X+Y, restart required)
    if ((v = opt_get(XENIA_OPT_DRAW_RESOLUTION_SCALE))) {
        int scale = atoi(v);
        if (scale >= 1 && scale <= 8) {
            cvars::draw_resolution_scale_x = scale;
            cvars::draw_resolution_scale_y = scale;
        }
    }

    // Anisotropic filtering override
    if ((v = opt_get(XENIA_OPT_ANISOTROPIC_FILTERING))) {
        cvars::anisotropic_override = atoi(v);
    }

    // Async shader compilation
    if ((v = opt_get(XENIA_OPT_ASYNC_SHADERS))) {
        cvars::async_shader_compilation = (strcmp(v, "enabled") == 0);
    }

    // Readback resolve
    if ((v = opt_get(XENIA_OPT_READBACK_RESOLVE))) {
        cvars::readback_resolve = (strcmp(v, "disabled") != 0);
    }

    // Store shaders
    if ((v = opt_get(XENIA_OPT_STORE_SHADERS))) {
        cvars::store_shaders = (strcmp(v, "enabled") == 0);
    }

    // Half-pixel offset
    if ((v = opt_get(XENIA_OPT_HALF_PIXEL_OFFSET))) {
        cvars::half_pixel_offset = (strcmp(v, "enabled") == 0);
    }

    // GPU allow invalid fetch constants
    if ((v = opt_get(XENIA_OPT_GPU_INVALID_FETCH))) {
        cvars::gpu_allow_invalid_fetch_constants = (strcmp(v, "enabled") == 0);
    }

    // Fuzzy alpha epsilon (NVIDIA fix)
    if ((v = opt_get(XENIA_OPT_FUZZY_ALPHA_EPSILON))) {
        cvars::use_fuzzy_alpha_epsilon = (strcmp(v, "enabled") == 0);
    }

    // Emulated display refresh rate: 60/50 Hz, 120 Hz (two vblanks a frame),
    // or uncapped (vblanks as fast as possible, not paced by RetroArch)
    if ((v = opt_get(XENIA_OPT_WAIT_FOR_FRAME)))
        core_state.wait_for_frame = strcmp(v, "enabled") == 0;

    if ((v = opt_get(XENIA_OPT_DISPLAY_REFRESH))) {
        // Unlock Framerate, whatever the Display Refresh Rate says: the
        // running game at the rate chosen, or (Listed games) at the one the
        // unlock list gives its title, if it lists it (NNshi)
        if (const char *u = opt_get(XENIA_OPT_UNLOCK_FRAMERATE);
            u && strcmp(u, "enabled") == 0) {
            switch (gamedb::unlock_for(xenia_root_dir(), core_state.title_id)) {
            case gamedb::Unlock::k30to60:   v = "30to60";   break;
            case gamedb::Unlock::k60to120:  v = "120";      break;
            case gamedb::Unlock::kUncapped: v = "uncapped"; break;
            case gamedb::Unlock::kNone:     break;
            }
        } else if (u && (strcmp(u, "30to60") == 0 || strcmp(u, "120") == 0 ||
                         strcmp(u, "uncapped") == 0)) {
            v = u;
        }
        const bool uncapped = strcmp(v, "uncapped") == 0;
        const bool pal = strcmp(v, "50") == 0;
        const bool changed = pal != core_state.pal_mode;
        core_state.vsync_enabled = !uncapped;
        SetGuestDisplayRefreshCap(core_state.vsync_enabled);
        core_state.vblanks_per_run = strcmp(v, "30to60") == 0 ? 2 : 1;
        const bool fps120 = strcmp(v, "120") == 0;
        core_state.fps120 = fps120;
        // Uncapped is told RetroArch's own refresh rate, so RetroArch runs the
        // core as often as the display shows frames instead of 60 times a
        // second (NNshi); 180 when RetroArch does not say
        double output_fps = pal ? 50.0 : fps120 ? 120.0 : 60.0;
        if (uncapped) {
            float target = 0.0f;
            output_fps = 180.0;
            if (core_state.environ_cb(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE, &target) &&
                target > 60.0f)
                output_fps = target;
        }
        const bool fps_changed = output_fps != core_state.output_fps;
        core_state.output_fps = output_fps;
        core_state.pal_mode = pal;
        cvars::use_50Hz_mode = pal;
        // Each retro_run is a guest vblank (two at 120 Hz), so the guest runs
        // at whatever rate RetroArch calls the core; it has to be told the
        // new rate, or a change made while a game runs did nothing (NNshi)
        if ((changed || fps_changed) && game_loaded) {
            struct retro_system_av_info av;
            retro_get_system_av_info(&av);
            core_state.environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        }
        xenia_log(RETRO_LOG_INFO, "emulated display refresh rate: %s\n", v);
    }

    // =================================================================
    // Video
    // =================================================================

    // Internal display resolution (restart required)
    if ((v = opt_get(XENIA_OPT_INTERNAL_DISPLAY_RES))) {
        cvars::internal_display_resolution = (uint32_t)atoi(v);
    }

    // Widescreen
    if ((v = opt_get(XENIA_OPT_WIDESCREEN))) {
        cvars::widescreen = (strcmp(v, "enabled") == 0);
    }

    // Video standard (1=NTSC, 2=NTSC-J, 3=PAL)
    if ((v = opt_get(XENIA_OPT_VIDEO_STANDARD))) {
        cvars::video_standard = atoi(v);
    }

    // Display gamma type (0=linear, 1=sRGB, 2=BT.709)
    if ((v = opt_get(XENIA_OPT_DISPLAY_GAMMA))) {
        cvars::kernel_display_gamma_type = (uint32_t)atoi(v);
    }

    // =================================================================
    // Audio
    // =================================================================

    // Audio enabled (controls update_audio)
    if ((v = opt_get(XENIA_OPT_AUDIO_ENABLED)))
        core_state.audio_enabled = (strcmp(v, "enabled") == 0);

    // Mute
    if ((v = opt_get(XENIA_OPT_MUTE))) {
        // Upstream mutes through its volume now
        static uint32_t unmuted_volume = 100;
        if (strcmp(v, "enabled") == 0) {
            if (cvars::volume) unmuted_volume = cvars::volume;
            cvars::volume = 0;
        } else if (!cvars::volume) {
            cvars::volume = unmuted_volume;
        }
    }

    // XMA decoder (restart required)
    if ((v = opt_get(XENIA_OPT_XMA_DECODER))) {
        cvars::xma_decoder = v;
    }

    // Dedicated XMA thread
    if ((v = opt_get(XENIA_OPT_DEDICATED_XMA_THREAD))) {
        cvars::use_dedicated_xma_thread = (strcmp(v, "enabled") == 0);
    }

    // Enable XMP (music player)
    if ((v = opt_get(XENIA_OPT_ENABLE_XMP))) {
        cvars::enable_xmp = (strcmp(v, "enabled") == 0);
    }

    // XMP default volume
    if ((v = opt_get(XENIA_OPT_XMP_DEFAULT_VOLUME))) {
        cvars::xmp_default_volume = atoi(v);
    }

    // =================================================================
    // Emulation
    // =================================================================

    // Time scalar
    if ((v = opt_get(XENIA_OPT_TIME_SCALAR))) {
        double scalar = atof(v);
        if (scalar > 0.0) {
            xe::Clock::set_guest_time_scalar(scalar);
        }
    }

    // Title updates
    if ((v = opt_get(XENIA_OPT_TITLE_UPDATES))) {
        cvars::apply_title_update = (strcmp(v, "enabled") == 0);
    }

    // Apply game patches
    if ((v = opt_get(XENIA_OPT_APPLY_PATCHES))) {
        cvars::apply_patches = (strcmp(v, "enabled") == 0);
    }

    // License mask (0=None, 1=Full, -1=All)
    if ((v = opt_get(XENIA_OPT_LICENSE_MASK))) {
        cvars::license_mask = atoi(v);
    }

    // User language
    if ((v = opt_get(XENIA_OPT_USER_LANGUAGE))) {
        // XLanguage IDs (xbox.h)
        static const std::pair<const char*, int32_t> languages[] = {
            {"English", 1}, {"Japanese", 2}, {"German", 3}, {"French", 4},
            {"Spanish", 5}, {"Italian", 6}, {"Korean", 7}, {"TChinese", 8},
            {"Portuguese", 9}, {"Polish", 11}, {"Russian", 12},
            {"SChinese", 17}};
        for (const auto& [name, id] : languages)
            if (strcmp(v, name) == 0)
                cvars::user_language = id;
    }

    // User country
    if ((v = opt_get(XENIA_OPT_USER_COUNTRY))) {
        // XConfig country IDs (user_country in kernel/xconfig.cc)
        static const std::pair<const char*, int32_t> countries[] = {
            {"United States", 103}, {"Great Britain", 35}, {"Japan", 53},
            {"Germany", 24}, {"France", 34}, {"Spain", 31}, {"Italy", 50},
            {"Australia", 6}, {"Canada", 16}, {"Brazil", 13}, {"Korea", 56},
            {"China", 20}, {"Mexico", 71}, {"Netherlands", 74},
            {"Russia", 88}, {"Sweden", 90}, {"Poland", 82},
            {"Portugal", 84}, {"Taiwan", 101}, {"Hong Kong", 39}};
        for (const auto& [name, id] : countries)
            if (strcmp(v, name) == 0)
                cvars::user_country = id;
    }

    // =================================================================
    // Compatibility
    // =================================================================

    // Protect zero page
    if ((v = opt_get(XENIA_OPT_PROTECT_ZERO))) {
        cvars::protect_zero = (strcmp(v, "enabled") == 0);
    }

    // Clear GPU memory page state
    if ((v = opt_get(XENIA_OPT_CLEAR_MEMORY_PAGE))) {
        cvars::clear_memory_page_state = (strcmp(v, "enabled") == 0);
    }

    // The settings xenia-manager's optimized settings change per game
    // (github.com/xenia-manager/optimized-settings), where Xenia Edge has them
    if ((v = opt_get(XENIA_OPT_OCCLUSION_QUERY)))
        cvars::occlusion_query = v;
    if ((v = opt_get(XENIA_OPT_OCCLUSION_FULL)))
        cvars::occlusion_query_full_counters = (strcmp(v, "enabled") == 0);
    if ((v = opt_get(XENIA_OPT_OCCLUSION_FAKE_LOWER)))
        cvars::occlusion_query_fake_lower_threshold = atoi(v);
    if ((v = opt_get(XENIA_OPT_OCCLUSION_FAKE_UPPER)))
        cvars::occlusion_query_fake_upper_threshold = atoi(v);
    if ((v = opt_get(XENIA_OPT_DEPTH_BIAS_SHADER)))
        cvars::depth_bias_shader_offset = (strcmp(v, "enabled") == 0);
    if ((v = opt_get(XENIA_OPT_SCALE_THRESHOLD)))
        cvars::draw_resolution_scale_threshold = static_cast<uint32_t>(atoi(v));
    if ((v = opt_get(XENIA_OPT_INVALID_UPLOAD)))
        cvars::gpu_allow_invalid_upload_range = (strcmp(v, "enabled") == 0);
    if ((v = opt_get(XENIA_OPT_GAMMA_UNORM16)))
        cvars::gamma_render_target_as_unorm16 = (strcmp(v, "enabled") == 0);
    if ((v = opt_get(XENIA_OPT_FORCE_DEPTH_CLAMP)))
        cvars::force_depth_clamp = (strcmp(v, "enabled") == 0);
    if ((v = opt_get(XENIA_OPT_IGNORE_RANGED_OFFSET)))
        cvars::ignore_offset_for_ranged_allocations = (strcmp(v, "enabled") == 0);
    if ((v = opt_get(XENIA_OPT_BREAK_UNIMPLEMENTED)))
        cvars::break_on_unimplemented_instructions = (strcmp(v, "enabled") == 0);
    if ((v = opt_get(XENIA_OPT_SCRIBBLE_HEAP)))
        cvars::scribble_heap = (strcmp(v, "enabled") == 0);
    if ((v = opt_get(XENIA_OPT_SCRIBBLE_HEAP_VALUE)))
        cvars::scribble_heap_value = atoi(v);

    // Disable context promotion
    if ((v = opt_get(XENIA_OPT_DISABLE_CTX_PROMOTION))) {
        cvars::disable_context_promotion = (strcmp(v, "enabled") == 0);
    }

    // Mount cache partition
    if ((v = opt_get(XENIA_OPT_MOUNT_CACHE))) {
        cvars::mount_cache = (strcmp(v, "enabled") == 0);
    }

    // Mount scratch partition
    if ((v = opt_get(XENIA_OPT_MOUNT_SCRATCH))) {
        cvars::mount_scratch = (strcmp(v, "enabled") == 0);
    }

    // =================================================================
    // Debug
    // =================================================================

    // Log level (0=error, 1=warning, 2=info, 3=debug)
    if ((v = opt_get(XENIA_OPT_LOG_LEVEL))) {
        int level = 2;
        if (strcmp(v, "error") == 0) level = 0;
        else if (strcmp(v, "warn") == 0) level = 1;
        else if (strcmp(v, "info") == 0) level = 2;
        else if (strcmp(v, "debug") == 0) level = 3;
        cvars::log_level = level;
    }

    // =================================================================
    // Per-game data
    // =================================================================

    // Patch Manager: a change is written to the game's patch file at once,
    // and Xenia applies it at the next boot
    if (!gamedb::patches().empty() && game_loaded)
        write_patch_files();
}

/* ================================================================== */
/*  Per-frame helpers                                                  */
/* ================================================================== */
static void update_audio(void) {
    if (!core_state.audio_enabled || !core_state.audio_buffer) return;
    if (!audio_mixer || !core_state.audio_batch_cb) return;

    // One frame of audio a call: 800 stereo frames at 48 kHz and 60 Hz (960 at 50). Up to
    // twice that, as before, was what a call took whenever the ring had a
    // backlog, which it always had; RetroArch's audio sync holds a call until
    // about what it was given has played, so every call took 33 ms - 30 calls
    // a second while the game swapped 60 times, and a ring kept full, about a
    // second behind (NNshi's run stats). The audio driver waits for room
    // instead, which paces the game's audio to playback.
    // 48000 a second over the output rate, the fraction carried over to the
    // next call (an uncapped rate like 165 does not divide 48000)
    static double s_sample_carry = 0.0;
    const double exact = 48000.0 / (core_state.output_fps > 0 ? core_state.output_fps : 60.0) + s_sample_carry;
    const size_t frames = static_cast<size_t>(exact);
    s_sample_carry = exact - static_cast<double>(frames);
    const size_t frame_samples = std::min(frames * s_audio_channels, core_state.audio_buffer_size);
    size_t got = audio_mixer->Mix(core_state.audio_buffer, frame_samples);
    if (got > 0 && s_audio_channels == 6)
        s_audio_multi.batch_int16(core_state.audio_buffer, got / 6, 6,
                                  RETRO_AUDIO_SPEAKER_FRONT_LEFT | RETRO_AUDIO_SPEAKER_FRONT_RIGHT |
                                  RETRO_AUDIO_SPEAKER_FRONT_CENTER | RETRO_AUDIO_SPEAKER_LOW_FREQUENCY |
                                  RETRO_AUDIO_SPEAKER_SIDE_LEFT | RETRO_AUDIO_SPEAKER_SIDE_RIGHT);
    else if (got > 0)
        core_state.audio_batch_cb(core_state.audio_buffer, got / 2);
}

static void update_video(void) {
    // Capture frame using presenter's CaptureGuestOutput.
    if (lr_graphics && lr_graphics->presenter()) {
        if (lr_graphics->presenter()->CaptureGuestOutput(captured_frame)) {
            if (captured_frame.width > 0 && captured_frame.height > 0 &&
                !captured_frame.data.empty()) {
                // RawImage R8G8B8X8 = XRGB8888 matches pixel format
                core_state.video_cb(captured_frame.data.data(),
                                    captured_frame.width,
                                    captured_frame.height,
                                    (unsigned)captured_frame.stride);
                return;
            }
        }
    }

    // Fallback: blank frame to prevent RetroArch hang.
    uint32_t w = 1280, h = 720;
    core_state.video_cb(nullptr, w, h, w * 4);
}

// The frame's image to RetroArch, which draws it this frame
static void vk_hand_over(VulkanFrameResources &f, uint32_t w, uint32_t h) {
    vk_current_image.image_view = f.image_view;
    vk_current_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&vk_current_image.create_info, 0, sizeof(vk_current_image.create_info));
    vk_current_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vk_current_image.create_info.image = f.image;
    vk_current_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vk_current_image.create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    vk_current_image.create_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY};
    vk_current_image.create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vk_hw->set_image(vk_hw->handle, &vk_current_image, 0, nullptr,
                     VK_QUEUE_FAMILY_IGNORED);

    core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0);
}

static void update_video_vulkan(void) {
    if (!vulkan_hw_render_active || !vk_hw) {
        update_video();
        return;
    }

    // Shared device: the guest output blitted straight into this frame's
    // image, GPU to GPU on the one device
    if (libretro_vk_shared_device_active()) {
        uint32_t sync_idx = vk_hw->get_sync_index(vk_hw->handle) &
                            vk_hw->get_sync_index_mask(vk_hw->handle);
        if (sync_idx >= VK_MAX_SYNC) sync_idx = 0;
        vk_hw->wait_sync_index(vk_hw->handle);
        VulkanFrameResources &f = vk_frames[sync_idx];
        uint32_t w = 0, h = 0;
        const bool blitted =
            lr_graphics && lr_graphics->presenter() &&
            libretro_vk_blit_guest_output(
                lr_graphics->presenter(),
                [&f](uint32_t fw, uint32_t fh) -> VkImage {
                    if (f.width != fw || f.height != fh) {
                        vk_destroy_frame(f);
                        if (!vk_create_frame(f, fw, fh)) return VK_NULL_HANDLE;
                    }
                    return f.image;
                },
                w, h);
        if (!blitted) {
            core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, 0, 0, 0);
            return;
        }
        vk_hand_over(f, w, h);
        return;
    }

    // Step 1: GPU blit capture (A2B10G10R10 ??? R8G8B8A8)
    const void* blit_data = nullptr;
    uint32_t w = 0, h = 0;
    if (!lr_graphics || !lr_graphics->presenter() ||
        !libretro_vk_capture_gpu_blit(lr_graphics->presenter(),
                                       blit_data, w, h) ||
        !blit_data || w == 0 || h == 0) {
        core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, 0, 0, 0);
        return;
    }

    // Step 2: Upload to frontend Vulkan image
    uint32_t sync_idx = vk_hw->get_sync_index(vk_hw->handle);
    uint32_t sync_mask = vk_hw->get_sync_index_mask(vk_hw->handle);
    sync_idx &= sync_mask;
    if (sync_idx >= VK_MAX_SYNC) sync_idx = 0;

    vk_hw->wait_sync_index(vk_hw->handle);

    VulkanFrameResources &f = vk_frames[sync_idx];

    // Recreate resources if dimensions changed
    if (f.width != w || f.height != h) {
        vk_destroy_frame(f);
        if (!vk_create_frame(f, w, h)) {
            xenia_log(RETRO_LOG_ERROR,
                      "Failed to create frontend Vulkan frame resources %ux%u\n", w, h);
            vulkan_hw_render_active = false;
            update_video();
            return;
        }
    }

    // Copy Xenia readback ??? frontend staging buffer
    memcpy(f.staging_mapped, blit_data, (size_t)w * h * 4);

    // Record commands: staging ??? image, transition to shader-read
    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo fe_begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    fe_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &fe_begin);

    VkImageMemoryBarrier fe_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    fe_barrier.srcAccessMask = 0;
    fe_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fe_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    fe_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    fe_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fe_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fe_barrier.image = f.image;
    fe_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &fe_barrier);

    VkBufferImageCopy fe_region = {};
    fe_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    fe_region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, f.staging_buffer, f.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &fe_region);

    fe_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fe_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    fe_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    fe_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &fe_barrier);

    vkEndCommandBuffer(cmd);

    vk_hw->lock_queue(vk_hw->handle);
    VkSubmitInfo fe_submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    fe_submit.commandBufferCount = 1;
    fe_submit.pCommandBuffers = &cmd;
    vkQueueSubmit(vk_hw->queue, 1, &fe_submit, VK_NULL_HANDLE);
    vk_hw->unlock_queue(vk_hw->handle);

    vk_hand_over(f, w, h);
}

#ifdef _WIN32
static void update_video_d3d12(void) {
    if (!d3d12_hw_render_active || !d3d12_hw) {
        update_video();
        return;
    }

    // Step 1: Capture with persistent resources (no per-frame alloc)
    const void* blit_data = nullptr;
    uint32_t w = 0, h = 0;
    if (!lr_graphics || !lr_graphics->presenter() ||
        !libretro_d3d12_capture_gpu_blit(lr_graphics->presenter(),
                                          blit_data, w, h) ||
        !blit_data || w == 0 || h == 0) {
        core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, 0, 0, 0);
        return;
    }

    // Step 2: Upload to frontend D3D12 texture
    ID3D12Device *device = d3d12_hw->device;

    // Pick next frame slot (double-buffered, avoid CPU-GPU stalls)
    D3D12FrameResources &f = d3d12_frames[d3d12_frame_idx];
    d3d12_frame_idx = (d3d12_frame_idx + 1) % D3D12_NUM_FRAMES;

    // Recreate resources if dimensions changed
    if (f.width != w || f.height != h) {
        // Wait for slot's prior work before destroying
        if (f.fence && f.fence_value > 0) {
            if (f.fence->GetCompletedValue() < f.fence_value) {
                f.fence->SetEventOnCompletion(f.fence_value, f.fence_event);
                WaitForSingleObject(f.fence_event, INFINITE);
            }
        }
        d3d12_destroy_frame(f);
        if (!d3d12_create_frame(f, device, w, h)) {
            xenia_log(RETRO_LOG_ERROR,
                      "Failed to create D3D12 frame resources %ux%u\n", w, h);
            d3d12_hw_render_active = false;
            update_video();
            return;
        }
    }

    // Wait for slot's prior GPU work (2 frames ago, usually done)
    if (f.fence_value > 0) {
        if (f.fence->GetCompletedValue() < f.fence_value) {
            f.fence->SetEventOnCompletion(f.fence_value, f.fence_event);
            WaitForSingleObject(f.fence_event, INFINITE);
        }
    }

    // Copy pixels to upload buffer (row-pitch aligned)
    D3D12_RESOURCE_DESC tex_desc = f.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &layout, nullptr, nullptr, nullptr);

    uint8_t *dst = (uint8_t *)f.upload_mapped + layout.Offset;
    const uint8_t *src = (const uint8_t *)blit_data;
    uint32_t src_pitch = w * 4;
    uint32_t dst_pitch = layout.Footprint.RowPitch;
    for (uint32_t row = 0; row < h; row++) {
        memcpy(dst + row * dst_pitch, src + row * src_pitch, src_pitch);
    }

    // Record commands
    f.cmd_alloc->Reset();
    f.cmd_list->Reset(f.cmd_alloc, nullptr);

    // Transition: COPY_SOURCE ??? COPY_DEST (after first frame)
    if (f.fence_value > 0) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = f.texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        f.cmd_list->ResourceBarrier(1, &barrier);
    }

    // Copy upload buffer ??? texture
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = f.texture;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = f.upload_buffer;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint = layout;

    f.cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    // Transition: COPY_DEST ??? COPY_SOURCE (required)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = f.texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        f.cmd_list->ResourceBarrier(1, &barrier);
    }

    f.cmd_list->Close();

    // Execute on the frontend's command queue
    ID3D12CommandList *lists[] = { f.cmd_list };
    d3d12_hw->queue->ExecuteCommandLists(1, lists);

    // Signal fence
    f.fence_value++;
    d3d12_hw->queue->Signal(f.fence, f.fence_value);

    // Pass the texture to the frontend
    d3d12_hw->set_texture(d3d12_hw->handle, f.texture, DXGI_FORMAT_R8G8B8A8_UNORM);

    // Signal to RetroArch that a HW frame is ready
    core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0);
}
#endif /* _WIN32 */

/* ================================================================== */
/*  Xenia lifecycle (C++ internal)                                     */
/* ================================================================== */

#ifdef _WIN32
// SEH wrapper (no C++ objects on stack).
#pragma warning(push)
#pragma warning(disable: 4611)  // setjmp interaction
static xe::X_STATUS xenia_launch_path_seh(const std::filesystem::path &p) {
    __try {
        return xenia_emulator->LaunchPath(p);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        xenia_log(RETRO_LOG_ERROR,
                  "SEH exception in LaunchPath! Code: 0x%08lX\n", code);
        return static_cast<xe::X_STATUS>(0x80000000 | code);
    }
}
#pragma warning(pop)
#else
static xe::X_STATUS xenia_launch_path_seh(const std::filesystem::path &p) {
    return xenia_emulator->LaunchPath(p);
}
#endif

#if defined(_WIN32) && defined(XENIA_LIBRETRO_DXIL)
// Writes the dxil.dll the core carries (libretro/CMakeLists.txt) to dir,
// unless the one there is already it
static void write_embedded_dxil(const std::filesystem::path &dir) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&write_embedded_dxil),
                            &module))
        return;
    HRSRC res = FindResourceW(module, L"XENIA_DXIL", MAKEINTRESOURCEW(10));
    HGLOBAL data = res ? LoadResource(module, res) : nullptr;
    const void *bytes = data ? LockResource(data) : nullptr;
    const DWORD size = res ? SizeofResource(module, res) : 0;
    if (!bytes || !size)
        return;
    const auto path = dir / "dxil.dll";
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) == size && !ec)
        return;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char *>(bytes), size);
    if (!out)
        xenia_log(RETRO_LOG_WARN, "Could not write %s\n",
                  path.string().c_str());
}
#endif

// A profile signed in to the first slot, as many titles ask for one before
// they start: standalone makes them in its UI, the core makes one itself,
// named after the frontend's username, and signs in the first it has after
// system/Xenia-Edge, set in xenia_setup_and_launch
static std::filesystem::path xenia_root;

// Guest swaps and vblanks (command_processor.cc), for the run stats
namespace xe { namespace gpu { extern std::atomic<uint64_t> g_guest_swaps, g_guest_vblanks; } }
using xe::gpu::g_guest_swaps;
using xe::gpu::g_guest_vblanks;

static void xenia_sign_in(void) {
    // system/Xenia-Edge/xbl_profilename.txt is there to be edited: made with
    // the default name whenever it is missing, never overwritten (NNshi)
    const std::filesystem::path name_file = xenia_root / "xbl_profilename.txt";
    {
        std::error_code ec;
        if (!std::filesystem::exists(name_file, ec)) {
            std::ofstream out(name_file);
            out << "Player\n";
        }
    }
    auto *profiles =
        xenia_emulator->kernel_state()->xam_state()->profile_manager();
    if (profiles->GetProfile(static_cast<uint8_t>(0)))
        return;
    if (profiles->GetAccountCount()) {
        profiles->Login(profiles->GetAccounts()->begin()->first, 0);
        return;
    }
    // The name from system/Xenia-Edge/xbl_profilename.txt, its first line,
    // rather than RetroArch's username (NNshi); "Player" when there is none.
    // A gamertag: letters, digits and single spaces, starting with a letter,
    // 15 at most
    std::string wanted;
    {
        std::ifstream in(name_file);
        std::getline(in, wanted);
    }
    std::string gamertag;
    const char *username = wanted.c_str();
    for (const char *c = username; c && *c && gamertag.size() < 15; ++c) {
        if (isalpha((unsigned char)*c) ||
            (!gamertag.empty() && isdigit((unsigned char)*c)))
            gamertag += *c;
        else if (*c == ' ' && !gamertag.empty() && gamertag.back() != ' ')
            gamertag += ' ';
    }
    while (!gamertag.empty() && gamertag.back() == ' ')
        gamertag.pop_back();
    if (!xe::kernel::xam::ProfileManager::IsGamertagValid(gamertag))
        gamertag = "Player";
    if (profiles->CreateProfile(gamertag, true))
        xenia_log(RETRO_LOG_INFO, "Created profile %s\n", gamertag.c_str());
    else
        xenia_log(RETRO_LOG_WARN, "Could not create a profile\n");
}

static bool xenia_setup_and_launch(const char *path) {
    try {
        namespace fs = std::filesystem;
        // Everything the emulator keeps - its storage, the content (saves,
        // title updates, DLC), caches and its log - in a folder of its own in
        // the system directory, as the other cores do (NNshi); nothing in the
        // frontend's own folder.
        fs::path root    = fs::path(core_state.system_dir) / "Xenia-Edge";
        xenia_root = root;
        fs::path storage = root;
        fs::path content = root / "content";
        fs::path cache   = root / "cache";
        fs::path cmdline = xe::to_path(std::string_view(path));  // RetroArch hands paths over in UTF-8

        std::error_code ec;
        fs::create_directories(cache, ec);
        fs::create_directories(content, ec);
        // Initialize Xenia logging first: its default file is beside the
        // executable, which for a core is the frontend's folder; and its errors
        // and warnings go to the frontend's log as well
#ifndef __ANDROID__
        if (cvars::log_file.empty())
            cvars::log_file = root / "xenia.log";
#endif
        // Once per process: a reset launches again in the same process, and a
        // second logger would be a second writer thread on the same file.
        static bool s_logging_initialized = false;
        if (!s_logging_initialized)
            xe::SetExtraLogSink(std::make_unique<RetroLogSink>());
#ifdef _WIN32
        // Direct3D 12's shader validator, dxil.dll, from the core's folder in
        // the system directory rather than beside the frontend's executable
        if (cvars::d3d12_runtime_path.empty())
            cvars::d3d12_runtime_path = root / "D3D12";
#ifdef XENIA_LIBRETRO_DXIL
        write_embedded_dxil(cvars::d3d12_runtime_path);
#endif
#endif
        if (!s_logging_initialized) {
#ifdef _WIN32
            xe::InitializeWin32App("xenia_libretro");
#else
            xe::InitializeLogging("xenia_libretro");
#endif
            s_logging_initialized = true;
        }
        XELOGI("Xenia Edge libretro core {}, built from commit {}",
               XENIA_LIBRETRO_VERSION, XENIA_LIBRETRO_COMMIT);

        xenia_emulator = std::make_unique<xe::Emulator>(
            cmdline, storage, content, cache);

        xe::X_STATUS status = xenia_emulator->Setup(
            /*display_window=*/nullptr,
            /*imgui_drawer=*/nullptr,
            /*require_cpu_backend=*/true,
            /*audio_system_factory=*/
            [](xe::cpu::Processor *processor)
                -> std::unique_ptr<xe::apu::AudioSystem> {
                auto sys = std::make_unique<xe::apu::libretro::LibretroAudioSystem>(processor);
                audio_mixer = sys.get();
                return sys;
            },
            /*graphics_system_factory=*/
            []() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
                std::unique_ptr<xe::gpu::GraphicsSystem> gs;
                if (strcmp(core_state.graphics_backend, "vulkan") == 0) {
                    gs = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
                }
#ifdef _WIN32
                else {
                    gs = std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>();
                }
#endif
                if (!gs) {
                    // Fallback to Vulkan if D3D12 unavailable
                    gs = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
                }
                lr_graphics = gs.get();
                return gs;
            },
            /*input_driver_factory=*/
            [](xe::ui::Window *window)
                -> std::vector<std::unique_ptr<xe::hid::InputDriver>> {
                std::vector<std::unique_ptr<xe::hid::InputDriver>> v;
                auto drv = std::make_unique<xe::hid::libretro_hid::LibretroInputDriver>(window, 0);
                lr_input_driver = drv.get();
                v.push_back(std::move(drv));
                return v;
            });

        if (XFAILED(status)) {
            xenia_log(RETRO_LOG_ERROR,
                      "Emulator::Setup failed 0x%08X\n", status);
            xenia_emulator.reset();
            return false;
        }

        // Setup only keeps the factories since upstream split it: graphics,
        // audio and the input drivers are made here, as the standalone app
        // does before it launches a title. Without it a title ran with no
        // graphics system and crashed on its first video call
        // (VdSetGraphicsInterruptCallback, NNshi: a black screen in every
        // game). The GPU uses its own Vulkan device, so the frontend's context
        // is not needed for this yet.
        // The drives the standalone app mounts right after Setup: scratch,
        // cache, cache0/cache1, the memory unit. Without them a game that
        // keeps streams in cache: failed to open them and crashed (NNshi,
        // Forza Horizon 2 at its first race: cache:\replay_stream).
        xenia_emulator->MountStandardDrives();

        status = xenia_emulator->SetupSubsystems();
        if (XFAILED(status)) {
            xenia_log(RETRO_LOG_ERROR,
                      "Emulator::SetupSubsystems failed 0x%08X\n", status);
            xenia_emulator.reset();
            return false;
        }

        xenia_sign_in();

        xenia_log(RETRO_LOG_INFO, "Launching %s\n", path);
        // Patch Manager: the game's module is loaded inside LaunchPath, on
        // this thread, and right before Xenia applies its patches the core
        // lists them for that module's hash and writes the files their
        // options say. Only the title's own module: the system modules
        // loaded after it go by.
        xe::kernel::xam::xam_message_box_notice =
            [](const std::string &title, const std::string &answer) {
                std::lock_guard<std::mutex> lock(s_notice_mutex);
                s_notice = title + ": " + answer;
            };
        // No window to ask for another disc in: Disc Control answers
        disc::set_notice([](const std::string &text) {
            std::lock_guard<std::mutex> lock(s_notice_mutex);
            s_notice = text;
        });
        xenia_emulator->set_disc_request(disc::request);
        static bool s_patches_listed;
        s_patches_listed = false;
        xe::kernel::before_title_patches =
            [](uint32_t title_id, std::optional<uint64_t> hash) {
                if (s_patches_listed || !title_id)
                    return;
                if (core_state.title_id && title_id != core_state.title_id)
                    return;
                s_patches_listed = true;
                if (!core_state.title_id) {
                    // Not read from the file: the unlock list goes by the
                    // module's title ID from here
                    core_state.title_id = title_id;
                    apply_core_options();
                }
                const bool had = !gamedb::patches().empty();
                if (!gamedb::find_patches(title_id, hash).empty() || had)
                    publish_core_options();
                if (!gamedb::patches().empty())
                    write_patch_files();
            };
        status = xenia_launch_path_seh(xe::to_path(std::string_view(path)));

        if (XFAILED(status)) {
            xenia_log(RETRO_LOG_ERROR,
                      "LaunchPath failed 0x%08X\n", status);
            xenia_emulator.reset();
            return false;
        }

        game_loaded = true;
        return true;

    } catch (const std::exception &e) {
        xenia_log(RETRO_LOG_ERROR, "Exception in setup: %s\n", e.what());
        xenia_emulator.reset();
        return false;
    } catch (...) {
        xenia_log(RETRO_LOG_ERROR, "Unknown exception in setup!\n");
        xenia_emulator.reset();
        return false;
    }
}

// RetroArch's pause (menu, pause key, a window losing focus) just stops
// calling retro_run, while the title's threads kept going on their own, so a
// game had run on by the time it was unpaused (NNshi). When retro_run has not
// come for 100 ms the guest is paused - no guest fiber dispatched, guest time
// standing still - and the next retro_run resumes it.
static std::thread s_pause_watchdog;
static std::mutex s_pause_mutex;
static std::condition_variable s_pause_cv;
static bool s_pause_watchdog_stop = false;
static std::atomic<bool> s_guest_paused{false};
static std::atomic<int64_t> s_last_run_ms{0};

static int64_t steady_ms(void) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::mutex s_pause_state_mutex;

static void guest_set_paused(bool paused) {
    // One at a time: the watchdog pausing and retro_run resuming must not
    // land in the scheduler in the opposite order
    std::lock_guard<std::mutex> lock(s_pause_state_mutex);
    if (s_guest_paused.exchange(paused) == paused)
        return;
    if (xenia_emulator && xenia_emulator->kernel_state() &&
        xenia_emulator->kernel_state()->guest_scheduler())
        xenia_emulator->kernel_state()->guest_scheduler()->SetPaused(paused);
    xe::Clock::SetGuestClockPaused(paused);
    xenia_log(RETRO_LOG_INFO, "%s\n", paused ? "paused (no retro_run for 100 ms)" : "resumed");
}

static void pause_watchdog_start(void) {
    s_last_run_ms = steady_ms();
    std::lock_guard<std::mutex> lock(s_pause_mutex);
    if (s_pause_watchdog.joinable())
        return;
    s_pause_watchdog_stop = false;
    s_pause_watchdog = std::thread([] {
        std::unique_lock<std::mutex> lock(s_pause_mutex);
        while (!s_pause_watchdog_stop) {
            s_pause_cv.wait_for(lock, std::chrono::milliseconds(25));
            if (!s_pause_watchdog_stop && game_loaded && !s_guest_paused &&
                steady_ms() - s_last_run_ms.load() > 100)
                guest_set_paused(true);
        }
    });
}

static void pause_watchdog_stop(void) {
    {
        std::lock_guard<std::mutex> lock(s_pause_mutex);
        s_pause_watchdog_stop = true;
    }
    s_pause_cv.notify_all();
    if (s_pause_watchdog.joinable())
        s_pause_watchdog.join();
}

// The emulator only; the frontend's context stays (a reset keeps it).
static void xenia_stop_emulator(void) {
    pause_watchdog_stop();
    if (xenia_emulator) {
        // Not paused while it stops: the title's threads have to run out
        guest_set_paused(false);
        // Not TerminateTitle: it ends the process, RetroArch with it
        if (xenia_emulator->is_title_open())
            xenia_emulator->StopTitleThreads();
        xenia_emulator->Shutdown();
        xenia_emulator.reset();
    }
    audio_mixer = nullptr;
    lr_input_driver = nullptr;
    lr_graphics = nullptr;
    game_loaded = false;
}

static void xenia_shutdown(void) {
    xenia_stop_emulator();

    // Clean up Vulkan HW render resources (frontend side)
    if (vk_hw) {
        vkDeviceWaitIdle(vk_hw->device);
        vk_destroy_all_frames();
    }
    vk_hw = nullptr;
    vulkan_hw_render_active = false;
    memset(&vk_current_image, 0, sizeof(vk_current_image));

    // Clean up D3D12 HW render resources
#ifdef _WIN32
    d3d12_destroy_all_frames();
    d3d12_hw = nullptr;
#endif
    d3d12_hw_render_active = false;
}

/* ================================================================== */
/*  Libretro API (extern "C")                                          */
/* ================================================================== */
extern "C" {

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

#ifdef _WIN32
// Whether a Direct3D 12 device can be made on the default adapter: the
// Graphics API option is offered, and D3D12 used, only then
static bool d3d12_available(void) {
    static const bool available = SUCCEEDED(D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
    return available;
}
#endif

RETRO_API void retro_set_environment(retro_environment_t cb) {
    core_state.environ_cb = cb;
    disc::set_environment(cb);

    struct retro_log_callback log_cb;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb)) {
        core_state.log_cb = log_cb.log;
    }

    // Publish core options v2
#ifdef _WIN32
    s_hide_graphics_api = !d3d12_available();
#endif
    publish_core_options();

    // Publish input descriptors for 4 Xbox 360 controllers
    static const struct retro_input_descriptor descs[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "A" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "B" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "X" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Y" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Back" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "LB" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "RB" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "LT" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "RT" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "LS" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "RS" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
        { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" },
        { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" },
        { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" },
        { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },
        { 0 },
    };
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)descs);

    const char *dir = nullptr;
    if (cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
        snprintf(core_state.system_dir, sizeof(core_state.system_dir), "%s", dir);
    dir = nullptr;
    if (cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
        snprintf(core_state.save_dir, sizeof(core_state.save_dir), "%s", dir);

}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)      { core_state.video_cb       = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)        { core_state.audio_cb       = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { core_state.audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)            { core_state.input_poll_cb  = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)          { core_state.input_state_cb = cb; }

RETRO_API void retro_init(void) {
    xenia_log(RETRO_LOG_INFO, "Xenia Edge libretro core %s, built from commit %s\n",
              XENIA_LIBRETRO_VERSION, XENIA_LIBRETRO_COMMIT);
    core_state.audio_sample_rate  = 48000.0;
    core_state.audio_buffer_size  = 6 * 4096;   // int16 values (4096 frames of 5.1)
    core_state.audio_buffer       = static_cast<int16_t *>(
        calloc(core_state.audio_buffer_size, sizeof(int16_t)));
    memset(core_state.graphics_backend, 0, sizeof(core_state.graphics_backend));
    core_state.vsync_enabled    = true;
    core_state.vblanks_per_run  = 1;
    core_state.fps120           = false;
    core_state.output_fps       = 60.0;
    core_state.audio_enabled    = true;
    core_state.pal_mode         = false;

}

RETRO_API void retro_deinit(void) {
    xenia_shutdown();

    free(core_state.audio_buffer);
    core_state.audio_buffer = nullptr;

}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "Xenia Edge";
    // Upstream's version and this core's commit, as rpcs3-libretro's: which
    // build a tester runs shows in RetroArch itself (NNshi)
    info->library_version  = XENIA_LIBRETRO_VERSION " (libretro core " XENIA_LIBRETRO_COMMIT ")";
    info->need_fullpath    = true;
    info->valid_extensions = "iso|xex|zar|xcp|m3u";
    info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = 1280;
    info->geometry.base_height  = 720;
    // The frame handed over is the guest output at the draw resolution
    // scale, up to 8x; the old fixed 3840x2160 (3x) was below it from 4x on
    const unsigned scale = std::max(1, std::min(8, int(cvars::draw_resolution_scale_x)));
    info->geometry.max_width    = 1280 * scale;
    info->geometry.max_height   = 720 * scale;
    info->geometry.aspect_ratio = 16.0f / 9.0f;
    info->timing.fps            = core_state.output_fps > 0 ? core_state.output_fps : 60.0;
    info->timing.sample_rate    = core_state.audio_sample_rate;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (lr_input_driver)
        lr_input_driver->SetPortDevice(port, device);
}

static struct retro_rumble_interface s_rumble = {};

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
    if (!info || !info->path) {
        xenia_log(RETRO_LOG_ERROR, "No game path supplied\n");
        return false;
    }
    // An .m3u is the game's discs; the game starts from its first (or the
    // one Disc Control had in last time), and asks for the others itself
    const std::string start_disc = disc::load(info->path);
    if (start_disc.empty()) {
        xenia_log(RETRO_LOG_ERROR, "No disc listed in %s\n", info->path);
        return false;
    }
    snprintf(core_state.game_path, sizeof(core_state.game_path),
             "%s", start_disc.c_str());

    // The game's title ID before anything is set up: the unlock list's rate
    // and the output rate RetroArch asks for right after this depend on it.
    // And the unlock list, made when missing.
    gamedb::install_files(xenia_root_dir());
    core_state.title_id = gamedb::title_id_of(xe::to_path(std::string_view(core_state.game_path)));
    xenia_log(RETRO_LOG_INFO, "title ID %08X\n", core_state.title_id);

    // Apply any options set before load
    apply_core_options();

    // Every core option's value, once, as standalone dumps its config: a
    // tester's log then says what the core was asked to do
    {
        std::string dump;
        for (const auto *d = xenia_core_options_v2_defs; d->key; ++d) {
            const char *value = opt_get(d->key);
            dump += std::string("\n  ") + d->key + " = " + (value ? value : "(default)");
        }
        xenia_log(RETRO_LOG_INFO, "core options:%s\n", dump.c_str());
    }

    // The backend is the Graphics API option's (NNshi), not whatever video
    // driver the frontend happens to run: the core asks for that context, and
    // RetroArch switches its driver to it. Following the frontend's driver
    // took D3D12 for anything not Vulkan - d3d11 or gl included - without a
    // context of that kind behind it.
    unsigned preferred_hw = RETRO_HW_CONTEXT_VULKAN;
    strncpy(core_state.graphics_backend, XENIA_GRAPHICS_VULKAN,
            sizeof(core_state.graphics_backend) - 1);
#ifdef _WIN32
    if (const char *api = opt_get(XENIA_OPT_GRAPHICS_API);
        api && strcmp(api, XENIA_GRAPHICS_D3D12) == 0 && d3d12_available()) {
        preferred_hw = RETRO_HW_CONTEXT_D3D12;
        strncpy(core_state.graphics_backend, XENIA_GRAPHICS_D3D12,
                sizeof(core_state.graphics_backend) - 1);
    }
#endif
    // Also update the gpu cvar so internal Xenia code stays consistent
    cvars::gpu = core_state.graphics_backend;
    xenia_log(RETRO_LOG_INFO, "Graphics API: %s\n", core_state.graphics_backend);

    // Request HW render from the frontend.
    memset(&hw_render_cb, 0, sizeof(hw_render_cb));
    hw_render_cb.depth = false;
    hw_render_cb.stencil = false;
    hw_render_cb.bottom_left_origin = false;
    hw_render_cb.cache_context = false;

    if (preferred_hw == RETRO_HW_CONTEXT_VULKAN) {
        hw_render_cb.context_type = RETRO_HW_CONTEXT_VULKAN;
        hw_render_cb.context_reset = vulkan_context_reset;
        hw_render_cb.context_destroy = vulkan_context_destroy;

        if (!core_state.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render_cb)) {
            xenia_log(RETRO_LOG_WARN,
                      "Frontend rejected Vulkan HW render ??? falling back to software\n");
            vulkan_hw_render_active = false;
        } else {
            static const struct retro_hw_render_context_negotiation_interface_vulkan negotiation = {
                RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
                RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
                xenia_vk_application_info,
                xenia_vk_create_device,
                xenia_vk_destroy_device,
            };
            core_state.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
                                  (void *)&negotiation);
            s_launch_on_context_reset = true;
        }
    }
#ifdef _WIN32
    else if (preferred_hw == RETRO_HW_CONTEXT_D3D12) {
        hw_render_cb.context_type = RETRO_HW_CONTEXT_D3D12;
        hw_render_cb.context_reset = d3d12_context_reset;
        hw_render_cb.context_destroy = d3d12_context_destroy;

        if (!core_state.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render_cb)) {
            xenia_log(RETRO_LOG_WARN,
                      "Frontend rejected D3D12 HW render ??? falling back to software\n");
            d3d12_hw_render_active = false;
        }
    }
#endif

    // Set pixel format (needed for software fallback and D3D12).
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!core_state.environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        xenia_log(RETRO_LOG_ERROR, "XRGB8888 not supported\n");
        return false;
    }

    // Acquire rumble interface from the frontend
    s_rumble = {};
    core_state.environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &s_rumble);

    // 5.1 when asked for and the frontend has multi-channel output; asked
    // once, at load, as the frontend wants it
    {
        const char *ch = opt_get(XENIA_OPT_AUDIO_CHANNELS);
        s_audio_multi = {};
        s_audio_channels = 2;
        if (ch && strcmp(ch, "5.1") == 0) {
            if (core_state.environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_MULTI, &s_audio_multi) &&
                s_audio_multi.batch_int16)
                s_audio_channels = 6;
            else
                xenia_log(RETRO_LOG_WARN, "audio: the frontend has no multi-channel output, staying stereo\n");
        }
    }
    xe::apu::libretro::LibretroAudioSystem::set_output_channels(s_audio_channels);
    xenia_log(RETRO_LOG_INFO, "audio: %u channels\n", s_audio_channels);

    // With Vulkan the game starts in context_reset, on RetroArch's device
    if (s_launch_on_context_reset)
        return true;
    launch_game();
    return game_loaded;
}

static void launch_game(void) {
    bool ok = xenia_setup_and_launch(core_state.game_path);

    // Now that the HID driver exists, give it the rumble callback
    if (ok && lr_input_driver && s_rumble.set_rumble_state)
        lr_input_driver->SetRumbleCallback(s_rumble.set_rumble_state);
    if (ok) {
        pause_watchdog_start();
    } else {
        struct retro_message msg = { "Xenia could not start the game - see the log", 600 };
        core_state.environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
}

RETRO_API void retro_unload_game(void) {
    // The game's patch files as its options leave them (NNshi)
    if (!gamedb::patches().empty())
        write_patch_files();
    disc::release();
    xenia_shutdown();
    disc::unload();
    core_state.title_id = 0;
    if (!gamedb::patches().empty()) {
        gamedb::forget_patches();
        publish_core_options();
    }
}

RETRO_API unsigned retro_get_region(void) {
    return core_state.pal_mode ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) {
    return false;
}

RETRO_API size_t retro_serialize_size(void) {
    return 0; // Save-states not yet supported for an Xbox 360 emulator
}
RETRO_API bool retro_serialize(void *, size_t) { return false; }
RETRO_API bool retro_unserialize(const void *, size_t) { return false; }

RETRO_API void *retro_get_memory_data(unsigned id) {
    if (!xenia_emulator || !game_loaded) return nullptr;
    if (id == RETRO_MEMORY_SYSTEM_RAM && xenia_emulator->memory())
        return xenia_emulator->memory()->physical_membase();
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    if (id == RETRO_MEMORY_SYSTEM_RAM)
        return 512u * 1024u * 1024u;  // Xbox 360: 512 MB unified
    return 0;
}

// A reset is a close and a load: the emulator is torn down the way unload
// does it and the same game launched again, with the frontend's context
// kept. It did nothing before (NNshi).
RETRO_API void retro_reset(void) {
    if (!xenia_emulator || !core_state.game_path[0])
        return;
    xenia_log(RETRO_LOG_INFO, "reset: relaunching %s\n", core_state.game_path);
    disc::release();
    xenia_stop_emulator();
    disc::resume();
    bool ok = xenia_setup_and_launch(core_state.game_path);
    if (ok) {
        if (lr_input_driver && s_rumble.set_rumble_state)
            lr_input_driver->SetRumbleCallback(s_rumble.set_rumble_state);
        pause_watchdog_start();
    } else {
        xenia_log(RETRO_LOG_ERROR, "reset: relaunch failed\n");
    }
}

// Where retro_run's time goes, once a second in the log, beside what the
// guest did in the same second (NNshi: RR6 at 30 fps where standalone holds
// 60): how often RetroArch calls it and how long it takes, how much of it is
// the GPU capture of the guest's frame, and the guest's swaps and vblanks.
// Diagnostic only.
static struct {
    std::chrono::steady_clock::time_point window_start, last_run;
    uint64_t runs = 0, swaps0 = 0, vblanks0 = 0;
    double run_ms = 0, run_max_ms = 0, gap_max_ms = 0, video_ms = 0, video_max_ms = 0;
} s_run_stats;

static double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

static void run_stats_end(std::chrono::steady_clock::time_point run_start, double video_ms) {
    auto &st = s_run_stats;
    const double ms = ms_since(run_start);
    st.runs++;
    st.run_ms += ms;
    st.run_max_ms = std::max(st.run_max_ms, ms);
    st.video_ms += video_ms;
    st.video_max_ms = std::max(st.video_max_ms, video_ms);
    const double window = std::chrono::duration<double>(std::chrono::steady_clock::now() - st.window_start).count();
    if (window < 1.0)
        return;
    const uint64_t swaps = g_guest_swaps.load(std::memory_order_relaxed);
    const uint64_t vblanks = g_guest_vblanks.load(std::memory_order_relaxed);
    xenia_log(RETRO_LOG_INFO,
              "[run stats] %.2f s: retro_run %llu (avg %.2f ms, max %.2f ms, longest gap %.2f ms); "
              "frame capture avg %.2f ms, max %.2f ms; guest swaps %llu, vblanks %llu\n",
              window, (unsigned long long)st.runs, st.run_ms / st.runs, st.run_max_ms, st.gap_max_ms,
              st.video_ms / st.runs, st.video_max_ms,
              (unsigned long long)(swaps - st.swaps0), (unsigned long long)(vblanks - st.vblanks0));
    st.runs = 0;
    st.run_ms = st.run_max_ms = st.gap_max_ms = st.video_ms = st.video_max_ms = 0;
    st.swaps0 = swaps;
    st.vblanks0 = vblanks;
    st.window_start = std::chrono::steady_clock::now();
}

RETRO_API void retro_run(void) {
    {
        std::string notice;
        {
            std::lock_guard<std::mutex> lock(s_notice_mutex);
            notice.swap(s_notice);
        }
        if (!notice.empty()) {
            struct retro_message msg = { notice.c_str(), 240 };
            core_state.environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
        }
    }
    const auto run_start = std::chrono::steady_clock::now();
    if (s_run_stats.window_start == std::chrono::steady_clock::time_point{})
        s_run_stats.window_start = run_start;
    if (s_run_stats.last_run != std::chrono::steady_clock::time_point{})
        s_run_stats.gap_max_ms = std::max(s_run_stats.gap_max_ms,
            std::chrono::duration<double, std::milli>(run_start - s_run_stats.last_run).count());
    s_run_stats.last_run = run_start;

    s_last_run_ms = steady_ms();
    if (s_guest_paused)
        guest_set_paused(false);
    // Input first, so the frame the vblank below starts reads this run's
    // input rather than the last one's
    if (core_state.input_poll_cb) core_state.input_poll_cb();

    // Feed libretro input state into Xenia's HID system
    if (lr_input_driver && core_state.input_state_cb)
        lr_input_driver->UpdateFromLibretro(core_state.input_state_cb);

    // One guest vblank per frontend frame (two at 120 Hz), with the refresh
    // rate not uncapped
    const uint64_t swaps_before = g_guest_swaps.load(std::memory_order_acquire);
    if (lr_graphics && core_state.vsync_enabled)
        for (int i = 0; i < core_state.vblanks_per_run; i++)
            lr_graphics->LibretroVblank();

    // Wait for the Game's Frame: the frame the game draws after this vblank,
    // handed over in this same run - at most most of a frame's time, so a
    // game that skips a frame costs no more than the frame it skips
    if (core_state.wait_for_frame && lr_graphics && core_state.vsync_enabled &&
        !s_guest_paused) {
        const double frame_ms = 1000.0 / (core_state.output_fps > 0 ? core_state.output_fps : 60.0);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::microseconds(static_cast<int64_t>(frame_ms * 800.0));
        while (g_guest_swaps.load(std::memory_order_acquire) == swaps_before &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    // Capture the latest frame via the appropriate video path.

    update_audio();
    const auto video_start = std::chrono::steady_clock::now();
    if (vulkan_hw_render_active)
        update_video_vulkan();
#ifdef _WIN32
    else if (d3d12_hw_render_active)
        update_video_d3d12();
#endif
    else
        update_video();
    const double video_ms = ms_since(video_start);

    // React to option changes
    bool vars_updated = false;
    if (core_state.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,
                              &vars_updated) && vars_updated) {
        apply_core_options();
    }

    run_stats_end(run_start, video_ms);
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char *) {}

} /* extern "C" */
