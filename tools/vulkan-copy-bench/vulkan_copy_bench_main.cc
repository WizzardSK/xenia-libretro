/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Times vkCmdCopyBufferToImage in isolation to settle whether the ~59 GB/s the
// texture cache reaches on Vulkan, against ~252 GB/s Direct3D 12 reaches for
// the same bytes, comes from the driver's buffer-to-image path or from how the
// texture cache creates its images.
//
// Deliberately shares nothing with the emulator: it links no xenia library, and
// the loader is opened by name with every entry point resolved through
// vkGetInstanceProcAddr. The cases are the six large uploads a 3x capture
// records, and the variants below them change one image property at a time so a
// slow baseline can be bisected.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "copy_shaders.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Every entry point this needs, so the executable links no Vulkan library.
#define XE_BENCH_INSTANCE_FUNCTIONS(X)   \
  X(vkCreateDevice)                      \
  X(vkEnumeratePhysicalDevices)          \
  X(vkGetDeviceProcAddr)                 \
  X(vkGetPhysicalDeviceFormatProperties) \
  X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceProperties)       \
  X(vkGetPhysicalDeviceQueueFamilyProperties)

#define XE_BENCH_DEVICE_FUNCTIONS(X) \
  X(vkAllocateCommandBuffers)        \
  X(vkAllocateMemory)                \
  X(vkBeginCommandBuffer)            \
  X(vkBindBufferMemory)              \
  X(vkBindImageMemory)               \
  X(vkCmdCopyBuffer)                 \
  X(vkCmdCopyBufferToImage)          \
  X(vkCmdCopyImage)                  \
  X(vkCmdPipelineBarrier)            \
  X(vkCmdResetQueryPool)             \
  X(vkCmdWriteTimestamp)             \
  X(vkCreateBuffer)                  \
  X(vkCreateCommandPool)             \
  X(vkCreateImage)                   \
  X(vkCreateComputePipelines)        \
  X(vkCreateDescriptorPool)          \
  X(vkCreateDescriptorSetLayout)     \
  X(vkCreateImageView)               \
  X(vkCreatePipelineLayout)          \
  X(vkCreateQueryPool)               \
  X(vkCreateShaderModule)            \
  X(vkAllocateDescriptorSets)        \
  X(vkCmdBindDescriptorSets)         \
  X(vkCmdBindPipeline)               \
  X(vkCmdDispatch)                   \
  X(vkCmdPushConstants)              \
  X(vkDestroyDescriptorPool)         \
  X(vkDestroyDescriptorSetLayout)    \
  X(vkDestroyImageView)              \
  X(vkDestroyPipeline)               \
  X(vkDestroyPipelineLayout)         \
  X(vkDestroyShaderModule)           \
  X(vkUpdateDescriptorSets)          \
  X(vkDestroyBuffer)                 \
  X(vkDestroyCommandPool)            \
  X(vkDestroyDevice)                 \
  X(vkDestroyImage)                  \
  X(vkDestroyQueryPool)              \
  X(vkEndCommandBuffer)              \
  X(vkFreeMemory)                    \
  X(vkGetBufferMemoryRequirements)   \
  X(vkGetDeviceQueue)                \
  X(vkGetImageMemoryRequirements)    \
  X(vkGetQueryPoolResults)           \
  X(vkQueueSubmit)                   \
  X(vkQueueWaitIdle)                 \
  X(vkResetCommandBuffer)

struct Functions {
#define XE_BENCH_DECLARE(name) PFN_##name name = nullptr;
  XE_BENCH_INSTANCE_FUNCTIONS(XE_BENCH_DECLARE)
  XE_BENCH_DEVICE_FUNCTIONS(XE_BENCH_DECLARE)
#undef XE_BENCH_DECLARE
};

void* OpenLoader() {
#if defined(_WIN32)
  return reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#elif defined(__APPLE__)
  void* handle = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
  return handle ? handle : dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
#else
  void* handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  return handle ? handle : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
}

void* LoaderSymbol(void* handle, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

// Mirrors ChooseMemoryType(..., MemoryPurpose::kDeviceLocal) in
// src/xenia/ui/vulkan/vulkan_util.h: lowest-indexed device-local type, and any
// type at all as the fallback.
uint32_t ChooseDeviceLocalMemoryType(
    const VkPhysicalDeviceMemoryProperties& memory_properties,
    uint32_t supported_types) {
  for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
    if ((supported_types & (uint32_t(1) << i)) &&
        (memory_properties.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      return i;
    }
  }
  for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
    if (supported_types & (uint32_t(1) << i)) {
      return i;
    }
  }
  return UINT32_MAX;
}

struct Context {
  Functions fn;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties = {};
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkQueryPool query_pool = VK_NULL_HANDLE;
  double timestamp_period_ns = 1.0;
  uint64_t timestamp_mask = ~uint64_t(0);
};

bool CreateBuffer(Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkBuffer& buffer_out, VkDeviceMemory& memory_out) {
  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (ctx.fn.vkCreateBuffer(ctx.device, &buffer_info, nullptr, &buffer_out) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements;
  ctx.fn.vkGetBufferMemoryRequirements(ctx.device, buffer_out, &requirements);
  uint32_t memory_type = ChooseDeviceLocalMemoryType(
      ctx.memory_properties, requirements.memoryTypeBits);
  if (memory_type == UINT32_MAX) {
    ctx.fn.vkDestroyBuffer(ctx.device, buffer_out, nullptr);
    return false;
  }
  VkMemoryAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = memory_type;
  if (ctx.fn.vkAllocateMemory(ctx.device, &allocate_info, nullptr,
                              &memory_out) != VK_SUCCESS) {
    ctx.fn.vkDestroyBuffer(ctx.device, buffer_out, nullptr);
    return false;
  }
  if (ctx.fn.vkBindBufferMemory(ctx.device, buffer_out, memory_out, 0) !=
      VK_SUCCESS) {
    ctx.fn.vkFreeMemory(ctx.device, memory_out, nullptr);
    ctx.fn.vkDestroyBuffer(ctx.device, buffer_out, nullptr);
    return false;
  }
  return true;
}

bool CreateImage(Context& ctx, uint32_t width, uint32_t height, VkFormat format,
                 VkImageUsageFlags usage, VkImageCreateFlags flags,
                 VkImage& image_out, VkDeviceMemory& memory_out) {
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.flags = flags;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent.width = width;
  image_info.extent.height = height;
  image_info.extent.depth = 1;
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (ctx.fn.vkCreateImage(ctx.device, &image_info, nullptr, &image_out) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements;
  ctx.fn.vkGetImageMemoryRequirements(ctx.device, image_out, &requirements);
  uint32_t memory_type = ChooseDeviceLocalMemoryType(
      ctx.memory_properties, requirements.memoryTypeBits);
  if (memory_type == UINT32_MAX) {
    ctx.fn.vkDestroyImage(ctx.device, image_out, nullptr);
    return false;
  }
  VkMemoryAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = memory_type;
  if (ctx.fn.vkAllocateMemory(ctx.device, &allocate_info, nullptr,
                              &memory_out) != VK_SUCCESS) {
    ctx.fn.vkDestroyImage(ctx.device, image_out, nullptr);
    return false;
  }
  if (ctx.fn.vkBindImageMemory(ctx.device, image_out, memory_out, 0) !=
      VK_SUCCESS) {
    ctx.fn.vkFreeMemory(ctx.device, memory_out, nullptr);
    ctx.fn.vkDestroyImage(ctx.device, image_out, nullptr);
    return false;
  }
  return true;
}

bool SubmitAndWait(Context& ctx) {
  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &ctx.command_buffer;
  if (ctx.fn.vkQueueSubmit(ctx.queue, 1, &submit_info, VK_NULL_HANDLE) !=
      VK_SUCCESS) {
    return false;
  }
  return ctx.fn.vkQueueWaitIdle(ctx.queue) == VK_SUCCESS;
}

bool TransitionImage(Context& ctx, VkImage image, VkImageLayout old_layout,
                     VkImageLayout new_layout) {
  ctx.fn.vkResetCommandBuffer(ctx.command_buffer, 0);
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (ctx.fn.vkBeginCommandBuffer(ctx.command_buffer, &begin_info) !=
      VK_SUCCESS) {
    return false;
  }
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask =
      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  ctx.fn.vkCmdPipelineBarrier(
      ctx.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  if (ctx.fn.vkEndCommandBuffer(ctx.command_buffer) != VK_SUCCESS) {
    return false;
  }
  return SubmitAndWait(ctx);
}

// Records one copy per submission with a timestamp on either side, so each
// measurement covers that copy alone. This is the same isolation RenderDoc
// imposes, which is what makes the numbers comparable to the capture.
template <typename RecordFn>
bool TimeCopy(Context& ctx, uint32_t iterations, RecordFn&& record,
              double& min_us_out, double& median_us_out) {
  std::vector<double> samples;
  samples.reserve(iterations);
  for (uint32_t i = 0; i < iterations + 1; ++i) {
    ctx.fn.vkResetCommandBuffer(ctx.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (ctx.fn.vkBeginCommandBuffer(ctx.command_buffer, &begin_info) !=
        VK_SUCCESS) {
      return false;
    }
    ctx.fn.vkCmdResetQueryPool(ctx.command_buffer, ctx.query_pool, 0, 2);
    ctx.fn.vkCmdWriteTimestamp(ctx.command_buffer,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               ctx.query_pool, 0);
    record(ctx.command_buffer);
    ctx.fn.vkCmdWriteTimestamp(ctx.command_buffer,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                               ctx.query_pool, 1);
    if (ctx.fn.vkEndCommandBuffer(ctx.command_buffer) != VK_SUCCESS) {
      return false;
    }
    if (!SubmitAndWait(ctx)) {
      return false;
    }
    uint64_t timestamps[2] = {};
    if (ctx.fn.vkGetQueryPoolResults(
            ctx.device, ctx.query_pool, 0, 2, sizeof(timestamps), timestamps,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
      return false;
    }
    // The first iteration warms caches and lets the driver settle.
    if (i == 0) {
      continue;
    }
    uint64_t begin = timestamps[0] & ctx.timestamp_mask;
    uint64_t end = timestamps[1] & ctx.timestamp_mask;
    samples.push_back(double(end - begin) * ctx.timestamp_period_ns / 1000.0);
  }
  if (samples.empty()) {
    return false;
  }
  std::sort(samples.begin(), samples.end());
  min_us_out = samples.front();
  median_us_out = samples[samples.size() / 2];
  return true;
}

struct Case {
  const char* name;
  uint32_t width;
  uint32_t height;
  VkFormat format;
  uint32_t texel_bytes;
  VkImageUsageFlags usage;
  VkImageCreateFlags flags;
};

void PrintResult(const char* label, uint64_t bytes, double min_us,
                 double median_us) {
  double gb_per_s = (double(bytes) / 1.0e9) / (min_us / 1.0e6);
  std::printf("  %-42s %11llu B  min %9.2f us  med %9.2f us  %7.1f GB/s\n",
              label, static_cast<unsigned long long>(bytes), min_us, median_us,
              gb_per_s);
}

bool RunBufferToImage(Context& ctx, const Case& c, uint32_t iterations) {
  uint64_t bytes = uint64_t(c.width) * c.height * c.texel_bytes;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
  if (!CreateBuffer(
          ctx, bytes,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          buffer, buffer_memory)) {
    std::printf("  %-42s buffer creation failed\n", c.name);
    return false;
  }
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory image_memory = VK_NULL_HANDLE;
  bool ok = CreateImage(ctx, c.width, c.height, c.format, c.usage, c.flags,
                        image, image_memory);
  if (ok) {
    ok = TransitionImage(ctx, image, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  }
  if (ok) {
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = c.width;
    region.bufferImageHeight = c.height;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = c.width;
    region.imageExtent.height = c.height;
    region.imageExtent.depth = 1;
    double min_us = 0.0;
    double median_us = 0.0;
    ok = TimeCopy(
        ctx, iterations,
        [&](VkCommandBuffer command_buffer) {
          ctx.fn.vkCmdCopyBufferToImage(command_buffer, buffer, image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                        &region);
        },
        min_us, median_us);
    if (ok) {
      PrintResult(c.name, bytes, min_us, median_us);
    }
  }
  if (!ok) {
    std::printf("  %-42s FAILED\n", c.name);
  }
  if (image != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyImage(ctx.device, image, nullptr);
  }
  if (image_memory != VK_NULL_HANDLE) {
    ctx.fn.vkFreeMemory(ctx.device, image_memory, nullptr);
  }
  ctx.fn.vkDestroyBuffer(ctx.device, buffer, nullptr);
  ctx.fn.vkFreeMemory(ctx.device, buffer_memory, nullptr);
  return ok;
}

// The alternative: a compute shader reading the same device-local buffer and
// writing the destination through imageStore. This is the shape the texture
// cache's load shaders would take if they wrote their image directly instead of
// detiling into a scratch buffer for vkCmdCopyBufferToImage to move.
bool RunComputeCopy(Context& ctx, const char* label, uint32_t width,
                    uint32_t height, VkFormat format, uint32_t texel_bytes,
                    const uint32_t* spirv, size_t spirv_bytes,
                    uint32_t iterations) {
  uint64_t bytes = uint64_t(width) * height * texel_bytes;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory image_memory = VK_NULL_HANDLE;
  VkImageView image_view = VK_NULL_HANDLE;
  VkShaderModule shader_module = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

  bool ok = CreateBuffer(ctx, bytes,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         buffer, buffer_memory) &&
            CreateImage(ctx, width, height, format,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        0, image, image_memory);
  if (ok) {
    ok = TransitionImage(ctx, image, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL);
  }
  if (ok) {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    ok = ctx.fn.vkCreateImageView(ctx.device, &view_info, nullptr,
                                  &image_view) == VK_SUCCESS;
  }
  if (ok) {
    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv_bytes;
    module_info.pCode = spirv;
    ok = ctx.fn.vkCreateShaderModule(ctx.device, &module_info, nullptr,
                                     &shader_module) == VK_SUCCESS;
  }
  if (ok) {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 2;
    set_layout_info.pBindings = bindings;
    ok = ctx.fn.vkCreateDescriptorSetLayout(ctx.device, &set_layout_info,
                                            nullptr, &set_layout) == VK_SUCCESS;
  }
  if (ok) {
    VkPushConstantRange push_range = {};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = sizeof(uint32_t) * 2;
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    ok = ctx.fn.vkCreatePipelineLayout(ctx.device, &pipeline_layout_info,
                                       nullptr, &pipeline_layout) == VK_SUCCESS;
  }
  if (ok) {
    VkComputePipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = shader_module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = pipeline_layout;
    ok = ctx.fn.vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1,
                                         &pipeline_info, nullptr,
                                         &pipeline) == VK_SUCCESS;
  }
  if (ok) {
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    ok = ctx.fn.vkCreateDescriptorPool(ctx.device, &pool_info, nullptr,
                                       &descriptor_pool) == VK_SUCCESS;
  }
  if (ok) {
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = descriptor_pool;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &set_layout;
    ok = ctx.fn.vkAllocateDescriptorSets(ctx.device, &allocate_info,
                                         &descriptor_set) == VK_SUCCESS;
  }
  if (ok) {
    VkDescriptorBufferInfo buffer_info = {};
    buffer_info.buffer = buffer;
    buffer_info.range = VK_WHOLE_SIZE;
    VkDescriptorImageInfo image_info = {};
    image_info.imageView = image_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptor_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &buffer_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptor_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &image_info;
    ctx.fn.vkUpdateDescriptorSets(ctx.device, 2, writes, 0, nullptr);

    uint32_t size[2] = {width, height};
    double min_us = 0.0;
    double median_us = 0.0;
    ok = TimeCopy(
        ctx, iterations,
        [&](VkCommandBuffer command_buffer) {
          ctx.fn.vkCmdBindPipeline(command_buffer,
                                   VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
          ctx.fn.vkCmdBindDescriptorSets(
              command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
              0, 1, &descriptor_set, 0, nullptr);
          ctx.fn.vkCmdPushConstants(command_buffer, pipeline_layout,
                                    VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(size), size);
          ctx.fn.vkCmdDispatch(command_buffer, (width + 7) / 8,
                               (height + 7) / 8, 1);
        },
        min_us, median_us);
    if (ok) {
      PrintResult(label, bytes, min_us, median_us);
    }
  }
  if (!ok) {
    std::printf("  %-42s FAILED\n", label);
  }
  if (descriptor_pool != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyDescriptorPool(ctx.device, descriptor_pool, nullptr);
  }
  if (pipeline != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyPipeline(ctx.device, pipeline, nullptr);
  }
  if (pipeline_layout != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
  }
  if (set_layout != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyDescriptorSetLayout(ctx.device, set_layout, nullptr);
  }
  if (shader_module != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyShaderModule(ctx.device, shader_module, nullptr);
  }
  if (image_view != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyImageView(ctx.device, image_view, nullptr);
  }
  if (image != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyImage(ctx.device, image, nullptr);
    ctx.fn.vkFreeMemory(ctx.device, image_memory, nullptr);
  }
  if (buffer != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyBuffer(ctx.device, buffer, nullptr);
    ctx.fn.vkFreeMemory(ctx.device, buffer_memory, nullptr);
  }
  return ok;
}

bool RunBufferToBuffer(Context& ctx, uint64_t bytes, uint32_t iterations) {
  VkBuffer src = VK_NULL_HANDLE;
  VkBuffer dst = VK_NULL_HANDLE;
  VkDeviceMemory src_memory = VK_NULL_HANDLE;
  VkDeviceMemory dst_memory = VK_NULL_HANDLE;
  bool ok = CreateBuffer(ctx, bytes,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         src, src_memory) &&
            CreateBuffer(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, dst,
                         dst_memory);
  if (ok) {
    VkBufferCopy region = {};
    region.size = bytes;
    double min_us = 0.0;
    double median_us = 0.0;
    ok = TimeCopy(
        ctx, iterations,
        [&](VkCommandBuffer command_buffer) {
          ctx.fn.vkCmdCopyBuffer(command_buffer, src, dst, 1, &region);
        },
        min_us, median_us);
    if (ok) {
      PrintResult("control: buffer -> buffer", bytes, min_us, median_us);
    }
  }
  if (!ok) {
    std::printf("  %-42s FAILED\n", "control: buffer -> buffer");
  }
  if (src != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyBuffer(ctx.device, src, nullptr);
    ctx.fn.vkFreeMemory(ctx.device, src_memory, nullptr);
  }
  if (dst != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyBuffer(ctx.device, dst, nullptr);
    ctx.fn.vkFreeMemory(ctx.device, dst_memory, nullptr);
  }
  return ok;
}

bool RunImageToImage(Context& ctx, const Case& c, uint32_t iterations) {
  uint64_t bytes = uint64_t(c.width) * c.height * c.texel_bytes;
  VkImage src = VK_NULL_HANDLE;
  VkImage dst = VK_NULL_HANDLE;
  VkDeviceMemory src_memory = VK_NULL_HANDLE;
  VkDeviceMemory dst_memory = VK_NULL_HANDLE;
  bool ok =
      CreateImage(ctx, c.width, c.height, c.format,
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  0, src, src_memory) &&
      CreateImage(ctx, c.width, c.height, c.format, c.usage, c.flags, dst,
                  dst_memory);
  if (ok) {
    ok = TransitionImage(ctx, src, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) &&
         TransitionImage(ctx, dst, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  }
  if (ok) {
    VkImageCopy region = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent.width = c.width;
    region.extent.height = c.height;
    region.extent.depth = 1;
    double min_us = 0.0;
    double median_us = 0.0;
    ok = TimeCopy(
        ctx, iterations,
        [&](VkCommandBuffer command_buffer) {
          ctx.fn.vkCmdCopyImage(
              command_buffer, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        },
        min_us, median_us);
    if (ok) {
      PrintResult("control: image -> image", bytes, min_us, median_us);
    }
  }
  if (!ok) {
    std::printf("  %-42s FAILED\n", "control: image -> image");
  }
  if (src != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyImage(ctx.device, src, nullptr);
    ctx.fn.vkFreeMemory(ctx.device, src_memory, nullptr);
  }
  if (dst != VK_NULL_HANDLE) {
    ctx.fn.vkDestroyImage(ctx.device, dst, nullptr);
    ctx.fn.vkFreeMemory(ctx.device, dst_memory, nullptr);
  }
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t device_index = 0;
  uint32_t iterations = 50;
  for (int i = 1; i < argc; ++i) {
    if (!std::strncmp(argv[i], "--device=", 9)) {
      device_index = uint32_t(std::atoi(argv[i] + 9));
    } else if (!std::strncmp(argv[i], "--iters=", 8)) {
      iterations = uint32_t(std::atoi(argv[i] + 8));
    } else {
      std::printf("usage: %s [--device=N] [--iters=N]\n", argv[0]);
      return 1;
    }
  }
  if (!iterations) {
    iterations = 1;
  }

  void* loader = OpenLoader();
  if (!loader) {
    std::printf("Failed to open the Vulkan loader.\n");
    return 1;
  }
  auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      LoaderSymbol(loader, "vkGetInstanceProcAddr"));
  auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
      get_instance_proc_addr
          ? get_instance_proc_addr(VK_NULL_HANDLE, "vkCreateInstance")
          : nullptr);
  if (!create_instance) {
    std::printf("Failed to resolve vkCreateInstance.\n");
    return 1;
  }

  VkApplicationInfo application_info = {};
  application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application_info.pApplicationName = "xenia-vulkan-copy-bench";
  application_info.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo instance_info = {};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &application_info;
  VkInstance instance = VK_NULL_HANDLE;
  if (create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
    std::printf("vkCreateInstance failed.\n");
    return 1;
  }

  Context ctx;
#define XE_BENCH_LOAD_INSTANCE(name)                                         \
  ctx.fn.name =                                                              \
      reinterpret_cast<PFN_##name>(get_instance_proc_addr(instance, #name)); \
  if (!ctx.fn.name) {                                                        \
    std::printf("Failed to resolve %s.\n", #name);                           \
    return 1;                                                                \
  }
  XE_BENCH_INSTANCE_FUNCTIONS(XE_BENCH_LOAD_INSTANCE)
#undef XE_BENCH_LOAD_INSTANCE

  uint32_t physical_device_count = 0;
  ctx.fn.vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
  if (!physical_device_count) {
    std::printf("No Vulkan devices.\n");
    return 1;
  }
  std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
  ctx.fn.vkEnumeratePhysicalDevices(instance, &physical_device_count,
                                    physical_devices.data());
  if (device_index >= physical_device_count) {
    std::printf("--device=%u out of range, %u present.\n", device_index,
                physical_device_count);
    return 1;
  }
  ctx.physical_device = physical_devices[device_index];

  VkPhysicalDeviceProperties device_properties;
  ctx.fn.vkGetPhysicalDeviceProperties(ctx.physical_device, &device_properties);
  ctx.fn.vkGetPhysicalDeviceMemoryProperties(ctx.physical_device,
                                             &ctx.memory_properties);
  ctx.timestamp_period_ns = double(device_properties.limits.timestampPeriod);

  std::printf("Device %u: %s\n", device_index, device_properties.deviceName);
  std::printf("  timestampPeriod = %.3f ns\n", ctx.timestamp_period_ns);
  std::printf("  memory types:\n");
  for (uint32_t i = 0; i < ctx.memory_properties.memoryTypeCount; ++i) {
    const VkMemoryType& type = ctx.memory_properties.memoryTypes[i];
    std::printf(
        "    [%u] heap %u  size %6llu MB  %s%s%s%s\n", i, type.heapIndex,
        static_cast<unsigned long long>(
            ctx.memory_properties.memoryHeaps[type.heapIndex].size >> 20),
        (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            ? "DEVICE_LOCAL "
            : "",
        (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            ? "HOST_VISIBLE "
            : "",
        (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            ? "HOST_COHERENT "
            : "",
        (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            ? "HOST_CACHED"
            : "");
  }

  uint32_t queue_family_count = 0;
  ctx.fn.vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical_device,
                                                  &queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  ctx.fn.vkGetPhysicalDeviceQueueFamilyProperties(
      ctx.physical_device, &queue_family_count, queue_families.data());
  uint32_t queue_family_index = UINT32_MAX;
  for (uint32_t i = 0; i < queue_family_count; ++i) {
    if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_index = i;
      break;
    }
  }
  if (queue_family_index == UINT32_MAX) {
    std::printf("No graphics queue family.\n");
    return 1;
  }
  uint32_t valid_bits = queue_families[queue_family_index].timestampValidBits;
  if (!valid_bits) {
    std::printf("Queue family %u reports no timestamp bits.\n",
                queue_family_index);
    return 1;
  }
  if (valid_bits < 64) {
    ctx.timestamp_mask = (uint64_t(1) << valid_bits) - 1;
  }
  std::printf("  queue family %u, %u timestamp bits\n", queue_family_index,
              valid_bits);

  float queue_priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = queue_family_index;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &queue_priority;
  VkDeviceCreateInfo device_info = {};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  if (ctx.fn.vkCreateDevice(ctx.physical_device, &device_info, nullptr,
                            &ctx.device) != VK_SUCCESS) {
    std::printf("vkCreateDevice failed.\n");
    return 1;
  }
#define XE_BENCH_LOAD_DEVICE(name)                    \
  ctx.fn.name = reinterpret_cast<PFN_##name>(         \
      ctx.fn.vkGetDeviceProcAddr(ctx.device, #name)); \
  if (!ctx.fn.name) {                                 \
    std::printf("Failed to resolve %s.\n", #name);    \
    return 1;                                         \
  }
  XE_BENCH_DEVICE_FUNCTIONS(XE_BENCH_LOAD_DEVICE)
#undef XE_BENCH_LOAD_DEVICE

  ctx.fn.vkGetDeviceQueue(ctx.device, queue_family_index, 0, &ctx.queue);
  VkCommandPoolCreateInfo command_pool_info = {};
  command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  command_pool_info.queueFamilyIndex = queue_family_index;
  if (ctx.fn.vkCreateCommandPool(ctx.device, &command_pool_info, nullptr,
                                 &ctx.command_pool) != VK_SUCCESS) {
    std::printf("vkCreateCommandPool failed.\n");
    return 1;
  }
  VkCommandBufferAllocateInfo command_buffer_info = {};
  command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_buffer_info.commandPool = ctx.command_pool;
  command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_info.commandBufferCount = 1;
  if (ctx.fn.vkAllocateCommandBuffers(ctx.device, &command_buffer_info,
                                      &ctx.command_buffer) != VK_SUCCESS) {
    std::printf("vkAllocateCommandBuffers failed.\n");
    return 1;
  }
  VkQueryPoolCreateInfo query_pool_info = {};
  query_pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query_pool_info.queryCount = 2;
  if (ctx.fn.vkCreateQueryPool(ctx.device, &query_pool_info, nullptr,
                               &ctx.query_pool) != VK_SUCCESS) {
    std::printf("vkCreateQueryPool failed.\n");
    return 1;
  }

  const VkImageUsageFlags kXeniaUsage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  // The six large uploads a 3x capture records, with the image properties the
  // texture cache gives them.
  const Case kCaptureCases[] = {
      {"capture 3360x1752 RGBA16F", 3360, 1752, VK_FORMAT_R16G16B16A16_SFLOAT,
       8, kXeniaUsage, 0},
      {"capture 3840x2160 RGBA8 (mutable)", 3840, 2160,
       VK_FORMAT_R8G8B8A8_UNORM, 4, kXeniaUsage,
       VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT},
      {"capture 3360x1752 RGBA8 (mutable)", 3360, 1752,
       VK_FORMAT_R8G8B8A8_UNORM, 4, kXeniaUsage,
       VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT},
      {"capture 1536x1536 R32F", 1536, 1536, VK_FORMAT_R32_SFLOAT, 4,
       kXeniaUsage, 0},
      {"capture 1536x1536 R16G16 (mutable)", 1536, 1536, VK_FORMAT_R16G16_UNORM,
       4, kXeniaUsage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT},
  };

  // One image property changed at a time against the largest capture case, so a
  // slow baseline says which property causes it.
  const Case kVariantCases[] = {
      {"variant TRANSFER_DST only", 3360, 1752, VK_FORMAT_R16G16B16A16_SFLOAT,
       8, VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0},
      {"variant + MUTABLE_FORMAT", 3360, 1752, VK_FORMAT_R16G16B16A16_SFLOAT, 8,
       kXeniaUsage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT},
      {"variant + STORAGE (compression off)", 3360, 1752,
       VK_FORMAT_R16G16B16A16_SFLOAT, 8,
       kXeniaUsage | VK_IMAGE_USAGE_STORAGE_BIT, 0},
      {"variant RGBA8 same pixels", 3360, 1752, VK_FORMAT_R8G8B8A8_UNORM, 4,
       kXeniaUsage, 0},
  };

  std::printf("\n%u timed iterations per case, GB/s from the fastest.\n\n",
              iterations);

  std::printf("buffer -> image, as the texture cache issues it:\n");
  for (const Case& c : kCaptureCases) {
    RunBufferToImage(ctx, c, iterations);
  }

  std::printf("\nvariants on the 3360x1752 RGBA16F case:\n");
  for (const Case& c : kVariantCases) {
    VkFormatProperties format_properties;
    ctx.fn.vkGetPhysicalDeviceFormatProperties(ctx.physical_device, c.format,
                                               &format_properties);
    if ((c.usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
        !(format_properties.optimalTilingFeatures &
          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
      std::printf("  %-42s skipped, format has no storage support\n", c.name);
      continue;
    }
    RunBufferToImage(ctx, c, iterations);
  }

  std::printf("\ncompute shader writing the image instead of copying:\n");
  RunComputeCopy(ctx, "compute -> image, 8 bytes/texel", 3360, 1752,
                 VK_FORMAT_R16G16B16A16_UINT, 8, kCopy64Spirv,
                 sizeof(kCopy64Spirv), iterations);
  RunComputeCopy(ctx, "compute -> image, 4 bytes/texel", 3360, 1752,
                 VK_FORMAT_R32_UINT, 4, kCopy32Spirv, sizeof(kCopy32Spirv),
                 iterations);

  std::printf("\ncontrols at the same byte count:\n");
  RunBufferToBuffer(ctx, uint64_t(3360) * 1752 * 8, iterations);
  RunImageToImage(ctx, kCaptureCases[0], iterations);

  std::printf(
      "\nbuffer -> image near the buffer -> buffer control means the driver's\n"
      "copy path is fine and the emulator's image setup is the difference;\n"
      "far below it on every case means the path itself is the ceiling.\n");

  ctx.fn.vkDestroyQueryPool(ctx.device, ctx.query_pool, nullptr);
  ctx.fn.vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
  ctx.fn.vkDestroyDevice(ctx.device, nullptr);
  return 0;
}
