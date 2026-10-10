/*
 * VulkanPresenter wrapper for libretro - isolates Vulkan headers from libretro.cpp
 */

#include "xenia/ui/vulkan/vulkan_presenter.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_instance.h"
#include "xenia/ui/vulkan/vulkan_provider.h"

#include "libretro_vk_presenter.h"

bool libretro_vk_capture_gpu_blit(xe::ui::Presenter* presenter,
                                   const void*& data_out,
                                   uint32_t& width_out,
                                   uint32_t& height_out) {
    if (!presenter) return false;
    auto* vk_presenter =
        dynamic_cast<xe::ui::vulkan::VulkanPresenter*>(presenter);
    if (!vk_presenter) return false;
    return vk_presenter->CaptureGuestOutputGPUBlit(data_out, width_out,
                                                    height_out);
}

// ---- The device shared with RetroArch ------------------------------------

using xe::ui::vulkan::VulkanDevice;
using xe::ui::vulkan::VulkanInstance;
using xe::ui::vulkan::VulkanProvider;

// Kept for as long as RetroArch keeps the device - across game loads, as it
// caches its context - not for one emulator's lifetime.
static std::unique_ptr<VulkanInstance> s_shared_instance;
static std::unique_ptr<VulkanDevice> s_shared_device;

bool libretro_vk_create_shared_device(
    VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const char** required_extensions, unsigned required_extension_count,
    const VkPhysicalDeviceFeatures* required_features,
    VkPhysicalDevice& gpu_out, VkDevice& device_out, VkQueue& queue_out,
    uint32_t& queue_family_out) {
    libretro_vk_destroy_shared_device();

    // The instance's version: the one asked for in the application info,
    // capped at what the loader has
    uint32_t api_version = VK_MAKE_API_VERSION(0, 1, 0, 0);
    if (auto enumerate_version = PFN_vkEnumerateInstanceVersion(
            get_instance_proc_addr(nullptr, "vkEnumerateInstanceVersion"))) {
        enumerate_version(&api_version);
    }
    api_version = std::min(api_version,
                           VulkanDevice::kHighestUsedApiMinorVersion);

    s_shared_instance = VulkanInstance::CreateExternal(
        instance, get_instance_proc_addr, api_version);
    if (!s_shared_instance) return false;

    std::vector<VkPhysicalDevice> gpus;
    if (gpu != VK_NULL_HANDLE) {
        gpus.push_back(gpu);
    } else {
        s_shared_instance->EnumeratePhysicalDevices(gpus);
    }

    VulkanDevice::FrontendRequest request;
    request.extensions = required_extensions;
    request.extension_count = required_extension_count;
    request.features = required_features;
    auto get_surface_support = PFN_vkGetPhysicalDeviceSurfaceSupportKHR(
        get_instance_proc_addr(instance,
                               "vkGetPhysicalDeviceSurfaceSupportKHR"));
    for (VkPhysicalDevice candidate : gpus) {
        std::unique_ptr<VulkanDevice> device = VulkanDevice::CreateIfSupported(
            s_shared_instance.get(), candidate, true, false, &request);
        if (!device) continue;
        const uint32_t family = device->queue_family_graphics_compute();
        // RetroArch presents from the queue it gets
        VkBool32 present = VK_TRUE;
        if (surface != VK_NULL_HANDLE && get_surface_support) {
            get_surface_support(candidate, family, surface, &present);
        }
        if (!present) {
            XELOGW("Vulkan: the graphics queue cannot present to RetroArch's "
                   "surface, RetroArch makes its own device");
            continue;
        }
        VkQueue queue = VK_NULL_HANDLE;
        device->functions().vkGetDeviceQueue(
            device->device(), family, device->frontend_queue_index(), &queue);
        gpu_out = candidate;
        device_out = device->device();
        queue_out = queue;
        queue_family_out = family;
        XELOGI("Vulkan: device shared with RetroArch, RetroArch on queue {} "
               "of family {}{}",
               device->frontend_queue_index(), family,
               device->frontend_queue_index() == 0 ? " (shared with Xenia)"
                                                   : "");
        s_shared_device = std::move(device);
        VulkanProvider::SetExternal(s_shared_instance.get(),
                                    s_shared_device.get());
        return true;
    }
    s_shared_instance.reset();
    return false;
}

void libretro_vk_destroy_shared_device() {
    VulkanProvider::SetExternal(nullptr, nullptr);
    // Neither destroys its Vulkan object: RetroArch does
    s_shared_device.reset();
    s_shared_instance.reset();
}

bool libretro_vk_shared_device_active() { return s_shared_device != nullptr; }

void libretro_vk_set_shared_queue_lock(void (*lock)(void* user),
                                       void (*unlock)(void* user),
                                       void* user) {
    if (!s_shared_device || s_shared_device->frontend_queue_index() != 0)
        return;
    auto& queue = *s_shared_device->queue_families()
                       [s_shared_device->queue_family_graphics_compute()]
                           .queues[0];
    std::lock_guard<std::recursive_mutex> guard(queue.mutex);
    queue.external_lock = lock;
    queue.external_unlock = unlock;
    queue.external_lock_user = user;
}

bool libretro_vk_blit_guest_output(
    xe::ui::Presenter* presenter,
    const std::function<VkImage(uint32_t width, uint32_t height)>& target_for,
    uint32_t& width_out, uint32_t& height_out) {
    auto* vk_presenter =
        dynamic_cast<xe::ui::vulkan::VulkanPresenter*>(presenter);
    return vk_presenter &&
           vk_presenter->BlitGuestOutputToImage(target_for, width_out,
                                                height_out);
}
