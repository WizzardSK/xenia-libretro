/*
 * Thin wrapper around VulkanPresenter for the libretro core.
 * Isolates the Vulkan C++ header dependency from libretro.cpp.
 *
 * Uses Vulkan's types: include it after the Vulkan headers (libretro.cpp's
 * libretro_vulkan.h, Xenia's vulkan_api.h), which it does not include itself
 * so that each side keeps its own platform defines.
 */
#ifndef LIBRETRO_VK_PRESENTER_H
#define LIBRETRO_VK_PRESENTER_H

#include <cstdint>
#include <functional>

// Forward declare
namespace xe {
namespace ui { class Presenter; }
}

// GPU blit capture: A2B10G10R10 ??? R8G8B8A8 via VulkanPresenter.
bool libretro_vk_capture_gpu_blit(xe::ui::Presenter* presenter,
                                   const void*& data_out,
                                   uint32_t& width_out,
                                   uint32_t& height_out);

// ---- The device shared with RetroArch ------------------------------------
//
// RetroArch's context negotiation lets the core make the VkDevice RetroArch
// then uses. Made here with everything Xenia needs, on RetroArch's instance,
// Xenia renders on it too, and a frame goes to RetroArch as a blit between
// two images of the one device - no readback to host memory and upload back,
// which cost as much GPU time as a good part of the emulation (NNshi).

// From the negotiation's create_device. False leaves RetroArch to make a
// device of its own, and Xenia then makes its own as before.
bool libretro_vk_create_shared_device(
    VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const char** required_extensions, unsigned required_extension_count,
    const VkPhysicalDeviceFeatures* required_features,
    VkPhysicalDevice& gpu_out, VkDevice& device_out, VkQueue& queue_out,
    uint32_t& queue_family_out);

// From the negotiation's destroy_device, before RetroArch destroys it: the
// emulator is gone by then (unloaded).
void libretro_vk_destroy_shared_device();

bool libretro_vk_shared_device_active();

// RetroArch's queue lock, when the device has one queue for both: Xenia
// takes it around its own submissions to that queue.
void libretro_vk_set_shared_queue_lock(void (*lock)(void* user),
                                       void (*unlock)(void* user),
                                       void* user);

// The guest output blitted into an image of RetroArch's frame (shared device
// only); target_for gets the size and gives the image.
bool libretro_vk_blit_guest_output(
    xe::ui::Presenter* presenter,
    const std::function<VkImage(uint32_t width, uint32_t height)>& target_for,
    uint32_t& width_out, uint32_t& height_out);

#endif // LIBRETRO_VK_PRESENTER_H
