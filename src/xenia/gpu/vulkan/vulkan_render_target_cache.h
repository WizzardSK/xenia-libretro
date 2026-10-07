/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_RENDER_TARGET_CACHE_H_
#define XENIA_GPU_VULKAN_VULKAN_RENDER_TARGET_CACHE_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>

#include "xenia/base/hash.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/edram_dump_shader.h"
#include "xenia/gpu/edram_transfer_shader.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_texture_cache.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/single_layout_descriptor_set_pool.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

class VulkanRenderTargetCache final : public RenderTargetCache {
 public:
  union RenderPassKey {
    struct {
      // If emulating 2x as 4x, this is still 2x for simplicity of using this
      // field to make guest-related decisions. Render pass objects are not very
      // expensive, and their dependencies can't be shared between 2x-as-4x and
      // true 4x MSAA passes (framebuffers because render target cache render
      // targets are different for 2x and 4x guest MSAA, pipelines because the
      // sample mask will have 2 samples excluded for 2x-as-4x).
      // This has effect only on the attachments, but even in cases when there
      // are no attachments, it can be used to pass the sample count between
      // subsystems, for instance, to specify the desired number of samples to
      // use when there are no attachments in pipelines.
      // Also, without attachments, using separate render passes for different
      // sample counts ensures that if the variableMultisampleRate feature is
      // not supported, no draws with different rasterization sample counts end
      // up in one render pass.
      xenos::MsaaSamples msaa_samples : xenos::kMsaaSamplesBits;  // 2
      // << 0 is depth, << 1...4 is color.
      uint32_t depth_and_color_used : 1 + xenos::kMaxColorRenderTargets;  // 7
      // 0 for unused attachments.
      // If VK_FORMAT_D24_UNORM_S8_UINT is not supported, this must be kD24FS8
      // even for kD24S8.
      xenos::DepthRenderTargetFormat depth_format
          : xenos::kDepthRenderTargetFormatBits;  // 8
      xenos::ColorRenderTargetFormat color_0_view_format
          : xenos::kColorRenderTargetFormatBits;  // 12
      xenos::ColorRenderTargetFormat color_1_view_format
          : xenos::kColorRenderTargetFormatBits;  // 16
      xenos::ColorRenderTargetFormat color_2_view_format
          : xenos::kColorRenderTargetFormatBits;  // 20
      xenos::ColorRenderTargetFormat color_3_view_format
          : xenos::kColorRenderTargetFormatBits;    // 24
      uint32_t color_rts_use_transfer_formats : 1;  // 25
    };
    uint32_t key = 0;
    struct Hasher {
      size_t operator()(const RenderPassKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const RenderPassKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const RenderPassKey& other_key) const {
      return !(*this == other_key);
    }
    bool operator<(const RenderPassKey& other_key) const {
      return key < other_key.key;
    }
  };
  static_assert_size(RenderPassKey, sizeof(uint32_t));

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D host_extent{};
    Framebuffer() = default;
    Framebuffer(VkFramebuffer framebuffer, const VkExtent2D& host_extent)
        : framebuffer(framebuffer), host_extent(host_extent) {}
  };

  VulkanRenderTargetCache(const RegisterFile& register_file,
                          const Memory& memory, TraceWriter& trace_writer,
                          uint32_t draw_resolution_scale_x,
                          uint32_t draw_resolution_scale_y,
                          VulkanCommandProcessor& command_processor);
  ~VulkanRenderTargetCache();

  // Transient descriptor set layouts must be initialized in the command
  // processor.
  bool Initialize(uint32_t shared_memory_binding_count);
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  void CompletedSubmissionUpdated();
  void EndSubmission();

  Path GetPath() const override { return path_; }

  VkBuffer edram_buffer() const { return edram_buffer_; }

  // Performs the resolve to a shared memory area according to the current
  // register values, and also clears the render targets if needed. Must be in a
  // frame for calling. copy_dest_info_out, if not null, receives the
  // destination info with the format normalized to the xenos::TextureFormat
  // the copy was actually performed with (only meaningful when a nonzero
  // length was written).
  // written_scaled_out: whether the data went to the scaled resolve address
  // space rather than shared memory (native resolves don't).
  bool Resolve(const Memory& memory, VulkanSharedMemory& shared_memory,
               VulkanTextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out,
               reg::RB_COPY_DEST_INFO* copy_dest_info_out = nullptr,
               bool* written_scaled_out = nullptr);

  bool Update(bool is_rasterization_done,
              reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask,
              const Shader& vertex_shader) override;
  // Binding information for the last successful update.
  RenderPassKey last_update_render_pass_key() const {
    return last_update_render_pass_key_;
  }
  VkRenderPass last_update_render_pass() const {
    return last_update_render_pass_;
  }
  const Framebuffer* last_update_framebuffer() const {
    return last_update_framebuffer_;
  }

  // For VK_KHR_dynamic_rendering: fills in attachment info structures.
  // Returns the number of color attachments (may be less than max if trailing
  // slots are unused). depth_attachment and stencil_attachment are filled if
  // depth is used (check key.depth_and_color_used & 0b1).
  void GetLastUpdateRenderingAttachments(
      VkRenderingAttachmentInfo* color_attachments,
      uint32_t* color_attachment_count_out,
      VkRenderingAttachmentInfo* depth_attachment,
      VkRenderingAttachmentInfo* stencil_attachment) const;

  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  // Persistent data doesn't depend on this, so can be overriden by per-game
  // configuration.
  bool IsFixedRG16TruncatedToMinus1To1() const {
    // TODO(Triang3l): Not float16 condition.
    return GetPath() == Path::kHostRenderTargets &&
           !cvars::snorm16_render_target_full_range;
  }
  bool IsFixedRGBA16TruncatedToMinus1To1() const {
    // TODO(Triang3l): Not float16 condition.
    return GetPath() == Path::kHostRenderTargets &&
           !cvars::snorm16_render_target_full_range;
  }

  bool depth_unorm24_vulkan_format_supported() const {
    return depth_unorm24_vulkan_format_supported_;
  }
  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }
  bool gamma_render_target_as_unorm16() const {
    return gamma_render_target_as_unorm16_;
  }

  bool msaa_2x_attachments_supported() const {
    return msaa_2x_attachments_supported_;
  }
  bool msaa_2x_no_attachments_supported() const {
    return msaa_2x_no_attachments_supported_;
  }
  bool IsMsaa2xSupported(bool subpass_has_attachments) const {
    return subpass_has_attachments ? msaa_2x_attachments_supported_
                                   : msaa_2x_no_attachments_supported_;
  }

  // Returns the render pass object, or VK_NULL_HANDLE if failed to create.
  // A render pass managed by the render target cache may be ended and resumed
  // at any time (to allow for things like copying and texture loading).
  VkRenderPass GetHostRenderTargetsRenderPass(RenderPassKey key);
  VkRenderPass GetFragmentShaderInterlockRenderPass() const {
    assert_true(GetPath() == Path::kPixelShaderInterlock);
    return fsi_render_pass_;
  }

  VkFormat GetDepthVulkanFormat(xenos::DepthRenderTargetFormat format) const;
  VkFormat GetColorVulkanFormat(xenos::ColorRenderTargetFormat format) const;
  VkFormat GetColorOwnershipTransferVulkanFormat(
      xenos::ColorRenderTargetFormat format,
      bool* is_integer_out = nullptr) const;

 protected:
  bool IsGammaFormatHostStorageSeparate() const override;

  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;

  bool IsHostDepthEncodingDifferent(
      xenos::DepthRenderTargetFormat format) const override;

  void RequestPixelShaderInterlockBarrier() override;

 private:
  enum class EdramBufferUsage {
    // There's no need for combined fragment and compute usages.
    // With host render targets, the usual usage sequence is as follows:
    // - Optionally compute writes - host depth copy storing for EDRAM range
    //   ownership transfers.
    // - Optionally fragment reads - host depth copy storing for EDRAM range
    //   ownership transfers.
    // - Compute writes - copying from host render targets during resolving.
    // - Compute reads - writing to the shared memory during resolving.
    // With the render backend implementation based on fragment shader
    // interlocks, it's:
    // - Fragment reads and writes - depth / stencil and color operations.
    // - Compute reads - writing to the shared memory during resolving.
    // So, fragment reads and compute reads normally don't follow each other,
    // and there's no need to amortize the cost of a read > read barrier in an
    // exceptional situation by using a wider barrier in the normal scenario.

    // Host depth copy storing.
    kFragmentRead,
    // Fragment shader interlock depth / stencil and color operations.
    kFragmentReadWrite,
    // Resolve - copying to the shared memory.
    kComputeRead,
    // Resolve - copying from host render targets.
    kComputeWrite,
    // Trace recording.
    kTransferRead,
    // Trace playback.
    kTransferWrite,
  };

  enum class EdramBufferModificationStatus {
    // The values are ordered by how strong the barrier conditions are.
    // No uncommitted shader writes.
    kUnmodified,
    // Need to commit before the next fragment shader interlock usage with
    // overlap.
    kViaFragmentShaderInterlock,
    // Need to commit before any next fragment shader interlock usage.
    kViaUnordered,
  };

  enum ResolveCopyDescriptorSet : uint32_t {
    // Never changes.
    kResolveCopyDescriptorSetEdram,
    // Shared memory or a region in it.
    kResolveCopyDescriptorSetDest,

    kResolveCopyDescriptorSetCount,
  };

  struct ResolveCopyShaderCode {
    const uint32_t* unscaled;
    size_t unscaled_size_bytes;
    const uint32_t* scaled;
    size_t scaled_size_bytes;
  };

  static void GetEdramBufferUsageMasks(EdramBufferUsage usage,
                                       VkPipelineStageFlags& stage_mask_out,
                                       VkAccessFlags& access_mask_out);
  void UseEdramBuffer(EdramBufferUsage new_usage);
  void MarkEdramBufferModified(
      EdramBufferModificationStatus modification_status =
          EdramBufferModificationStatus::kViaUnordered);
  void CommitEdramBufferShaderWrites(
      EdramBufferModificationStatus commit_status =
          EdramBufferModificationStatus::kViaFragmentShaderInterlock);

  VulkanCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;

  Path path_ = Path::kHostRenderTargets;

  // Cached SPIR-V version based on device capabilities.
  unsigned int spirv_version_;

  // Accessible in fragment and compute shaders.
  VkDescriptorSetLayout descriptor_set_layout_storage_buffer_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_sampled_image_x2_ =
      VK_NULL_HANDLE;

  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool>
      descriptor_set_pool_sampled_image_;
  std::unique_ptr<ui::vulkan::SingleLayoutDescriptorSetPool>
      descriptor_set_pool_sampled_image_x2_;

  VkDeviceMemory edram_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer edram_buffer_ = VK_NULL_HANDLE;
  EdramBufferUsage edram_buffer_usage_;
  VkBuffer edram_snapshot_download_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory edram_snapshot_download_buffer_memory_ = VK_NULL_HANDLE;
  bool edram_snapshot_download_mapped_ = false;
  // Kept until shutdown so it outlives the copy it feeds - trace restore
  // happens once per replay.
  VkBuffer edram_snapshot_restore_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory edram_snapshot_restore_buffer_memory_ = VK_NULL_HANDLE;
  EdramBufferModificationStatus edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  VkDescriptorPool edram_storage_buffer_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet edram_storage_buffer_descriptor_set_;

  VkPipelineLayout resolve_copy_pipeline_layout_ = VK_NULL_HANDLE;
  static const ResolveCopyShaderCode
      kResolveCopyShaders[size_t(draw_util::ResolveCopyShaderIndex::kCount)];
  std::array<VkPipeline, size_t(draw_util::ResolveCopyShaderIndex::kCount)>
      resolve_copy_pipelines_{};
  // Unscaled variants for fully native resolves. Only created with resolution
  // scaling, otherwise the main set is already unscaled. A separate set
  // because the scaled shaders can't just run at 1x1, their push constants
  // and destination address space assume the scaled resolve buffer.
  VkPipelineLayout resolve_copy_native_pipeline_layout_ = VK_NULL_HANDLE;
  std::array<VkPipeline, size_t(draw_util::ResolveCopyShaderIndex::kCount)>
      resolve_copy_native_pipelines_{};

  // On the fragment shader interlock path, the render pass key is used purely
  // for passing parameters to pipeline setup - there's always only one render
  // pass.
  RenderPassKey last_update_render_pass_key_;
  VkRenderPass last_update_render_pass_ = VK_NULL_HANDLE;
  // The pitch is not used on the fragment shader interlock path.
  uint32_t last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  // The attachments are not used on the fragment shader interlock path.
  const RenderTarget* const*
      last_update_framebuffer_attachments_[1 + xenos::kMaxColorRenderTargets] =
          {};
  const Framebuffer* last_update_framebuffer_ = VK_NULL_HANDLE;

  // For host render targets.

  // Can only be destroyed when framebuffers referencing it are destroyed!
  class VulkanRenderTarget final : public RenderTarget {
   public:
    static constexpr VkPipelineStageFlags kColorDrawStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    static constexpr VkAccessFlags kColorDrawAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kColorDrawLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    static constexpr VkPipelineStageFlags kDepthDrawStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    static constexpr VkAccessFlags kDepthDrawAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    static constexpr VkImageLayout kDepthDrawLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Takes ownership of the Vulkan objects passed to the constructor.
    VulkanRenderTarget(RenderTargetKey key,
                       VulkanRenderTargetCache& render_target_cache,
                       VkImage image, VkDeviceMemory memory,
                       VkImageView view_depth_color,
                       VkImageView view_depth_stencil, VkImageView view_stencil,
                       VkImageView view_color_transfer_separate,
                       size_t descriptor_set_index_transfer_source)
        : RenderTarget(key),
          render_target_cache_(render_target_cache),
          image_(image),
          memory_(memory),
          view_depth_color_(view_depth_color),
          view_depth_stencil_(view_depth_stencil),
          view_stencil_(view_stencil),
          view_color_transfer_separate_(view_color_transfer_separate),
          descriptor_set_index_transfer_source_(
              descriptor_set_index_transfer_source) {}
    ~VulkanRenderTarget();

    VkImage image() const { return image_; }

    VkImageView view_depth_color() const { return view_depth_color_; }
    VkImageView view_depth_stencil() const { return view_depth_stencil_; }
    VkImageView view_color_transfer_separate() const {
      return view_color_transfer_separate_;
    }
    VkImageView view_color_transfer() const {
      return view_color_transfer_separate_ != VK_NULL_HANDLE
                 ? view_color_transfer_separate_
                 : view_depth_color_;
    }
    VkDescriptorSet GetDescriptorSetTransferSource() const {
      ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
          key().is_depth
              ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
              : *render_target_cache_.descriptor_set_pool_sampled_image_;
      return descriptor_set_pool.Get(descriptor_set_index_transfer_source_);
    }

    static void GetDrawUsage(bool is_depth,
                             VkPipelineStageFlags* stage_mask_out,
                             VkAccessFlags* access_mask_out,
                             VkImageLayout* layout_out) {
      if (stage_mask_out) {
        *stage_mask_out = is_depth ? kDepthDrawStageMask : kColorDrawStageMask;
      }
      if (access_mask_out) {
        *access_mask_out =
            is_depth ? kDepthDrawAccessMask : kColorDrawAccessMask;
      }
      if (layout_out) {
        *layout_out = is_depth ? kDepthDrawLayout : kColorDrawLayout;
      }
    }
    void GetDrawUsage(VkPipelineStageFlags* stage_mask_out,
                      VkAccessFlags* access_mask_out,
                      VkImageLayout* layout_out) const {
      GetDrawUsage(key().is_depth, stage_mask_out, access_mask_out, layout_out);
    }
    VkPipelineStageFlags current_stage_mask() const {
      return current_stage_mask_;
    }
    VkAccessFlags current_access_mask() const { return current_access_mask_; }
    VkImageLayout current_layout() const { return current_layout_; }
    void SetUsage(VkPipelineStageFlags stage_mask, VkAccessFlags access_mask,
                  VkImageLayout layout) {
      current_stage_mask_ = stage_mask;
      current_access_mask_ = access_mask;
      current_layout_ = layout;
    }

    uint32_t temporary_sort_index() const { return temporary_sort_index_; }
    void SetTemporarySortIndex(uint32_t index) {
      temporary_sort_index_ = index;
    }

   private:
    VulkanRenderTargetCache& render_target_cache_;

    VkImage image_;
    VkDeviceMemory memory_;

    // TODO(Triang3l): Per-format drawing views for mutable formats with EDRAM
    // aliasing without transfers.
    VkImageView view_depth_color_;
    // Optional views.
    VkImageView view_depth_stencil_;
    VkImageView view_stencil_;
    VkImageView view_color_transfer_separate_;

    // 2 sampled images for depth / stencil, 1 sampled image for color.
    size_t descriptor_set_index_transfer_source_;

    VkPipelineStageFlags current_stage_mask_ = 0;
    VkAccessFlags current_access_mask_ = 0;
    VkImageLayout current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    // Temporary storage for indices in operations like transfers and dumps.
    uint32_t temporary_sort_index_ = 0;
  };

  struct FramebufferKey {
    RenderPassKey render_pass_key;

    // Same as RenderTargetKey::pitch_tiles_at_32bpp.
    uint32_t pitch_tiles_at_32bpp : 8;  // 8
    // [0, 2047].
    uint32_t depth_base_tiles : xenos::kEdramBaseTilesBits;    // 19
    uint32_t color_0_base_tiles : xenos::kEdramBaseTilesBits;  // 30

    uint32_t color_1_base_tiles : xenos::kEdramBaseTilesBits;  // 43
    uint32_t color_2_base_tiles : xenos::kEdramBaseTilesBits;  // 54

    uint32_t color_3_base_tiles : xenos::kEdramBaseTilesBits;  // 75

    // Including all the padding, for a stable hash.
    FramebufferKey() { Reset(); }
    FramebufferKey(const FramebufferKey& key) {
      std::memcpy(this, &key, sizeof(*this));
    }
    FramebufferKey& operator=(const FramebufferKey& key) {
      std::memcpy(this, &key, sizeof(*this));
      return *this;
    }
    bool operator==(const FramebufferKey& key) const {
      return std::memcmp(this, &key, sizeof(*this)) == 0;
    }
    using Hasher = xe::hash::XXHasher<FramebufferKey>;
    void Reset() { std::memset(this, 0, sizeof(*this)); }
  };

  struct TransferPipelineKey {
    RenderPassKey render_pass_key;
    EdramTransferShaderKey shader_key;

    TransferPipelineKey(RenderPassKey render_pass_key,
                        EdramTransferShaderKey shader_key)
        : render_pass_key(render_pass_key), shader_key(shader_key) {}

    struct Hasher {
      size_t operator()(const TransferPipelineKey& key) const {
        XXH3_state_t hash_state;
        XXH3_64bits_reset(&hash_state);
        XXH3_64bits_update(&hash_state, &key.render_pass_key,
                           sizeof(key.render_pass_key));
        XXH3_64bits_update(&hash_state, &key.shader_key,
                           sizeof(key.shader_key));
        return static_cast<size_t>(XXH3_64bits_digest(&hash_state));
      }
    };
    bool operator==(const TransferPipelineKey& other_key) const {
      return render_pass_key == other_key.render_pass_key &&
             shader_key == other_key.shader_key;
    }
    bool operator!=(const TransferPipelineKey& other_key) const {
      return !(*this == other_key);
    }
    bool operator<(const TransferPipelineKey& other_key) const {
      if (render_pass_key != other_key.render_pass_key) {
        return render_pass_key < other_key.render_pass_key;
      }
      return shader_key < other_key.shader_key;
    }
  };

  struct TransferInvocation {
    Transfer transfer;
    EdramTransferShaderKey shader_key;
    TransferInvocation(const Transfer& transfer,
                       const EdramTransferShaderKey& shader_key)
        : transfer(transfer), shader_key(shader_key) {}
    bool operator<(const TransferInvocation& other_invocation) const {
      // TODO(Triang3l): See if it may be better to sort by the source in the
      // first place, especially when reading the same data multiple times (like
      // to write the stencil bits after depth) for better read locality.
      // Sort by the shader key primarily to reduce pipeline state (context)
      // switches.
      if (shader_key != other_invocation.shader_key) {
        return shader_key < other_invocation.shader_key;
      }
      // Host depth render targets are changed rarely if they exist, won't save
      // many binding changes, ignore them for simplicity (their existence is
      // caught by the shader key change).
      assert_not_null(transfer.source);
      assert_not_null(other_invocation.transfer.source);
      uint32_t source_index =
          static_cast<const VulkanRenderTarget*>(transfer.source)
              ->temporary_sort_index();
      uint32_t other_source_index = static_cast<const VulkanRenderTarget*>(
                                        other_invocation.transfer.source)
                                        ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other_invocation.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(
        const TransferInvocation& other_invocation) const {
      return shader_key == other_invocation.shader_key &&
             transfer.AreSourcesSame(other_invocation.transfer);
    }
  };

  enum DumpDescriptorSet : uint32_t {
    // Never changes. Same in both color and depth pipeline layouts, keep the
    // first for pipeline layout compatibility, to only have to set it once.
    kDumpDescriptorSetEdram,
    // One resolve may need multiple sources. Different descriptor set layouts
    // for color and depth.
    kDumpDescriptorSetSource,

    kDumpDescriptorSetCount,
  };

  struct DumpInvocation {
    ResolveCopyDumpRectangle rectangle;
    EdramDumpShaderKey pipeline_key;
    DumpInvocation(const ResolveCopyDumpRectangle& rectangle,
                   const EdramDumpShaderKey& pipeline_key)
        : rectangle(rectangle), pipeline_key(pipeline_key) {}
    bool operator<(const DumpInvocation& other_invocation) const {
      // Sort by the pipeline key primarily to reduce pipeline state (context)
      // switches.
      if (pipeline_key != other_invocation.pipeline_key) {
        return pipeline_key < other_invocation.pipeline_key;
      }
      assert_not_null(rectangle.render_target);
      uint32_t render_target_index =
          static_cast<const VulkanRenderTarget*>(rectangle.render_target)
              ->temporary_sort_index();
      const ResolveCopyDumpRectangle& other_rectangle =
          other_invocation.rectangle;
      uint32_t other_render_target_index =
          static_cast<const VulkanRenderTarget*>(other_rectangle.render_target)
              ->temporary_sort_index();
      if (render_target_index != other_render_target_index) {
        return render_target_index < other_render_target_index;
      }
      if (rectangle.row_first != other_rectangle.row_first) {
        return rectangle.row_first < other_rectangle.row_first;
      }
      return rectangle.row_first_start < other_rectangle.row_first_start;
    }
  };

  // Returns the framebuffer object, or VK_NULL_HANDLE if failed to create.
  const Framebuffer* GetHostRenderTargetsFramebuffer(
      RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
      const RenderTarget* const* depth_and_color_render_targets);

  VkShaderModule GetTransferShader(EdramTransferShaderKey key);
  // With sample-rate shading, returns a pointer to one pipeline. Without
  // sample-rate shading, returns a pointer to as many pipelines as there are
  // samples. If there was a failure to create a pipeline, returns nullptr.
  VkPipeline const* GetTransferPipelines(TransferPipelineKey key);

  // Do ownership transfers for render targets - each render target / vector may
  // be null / empty in case there's nothing to do for them.
  // resolve_clear_rectangle is expected to be provided by
  // PrepareHostRenderTargetsResolveClear which should do all the needed size
  // bound checks.
  void PerformTransfersAndResolveClears(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const uint64_t* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr);

  VkPipeline GetDumpPipeline(EdramDumpShaderKey key);

  // Writes contents of the host render targets within those same rectangles
  // straight into shared memory in the destination's guest texture layout,
  // skipping edram_buffer_ and the resolve copy that would read it back again.
  // Returns false without encoding anything if it can't, leaving the caller to
  // fall back to the round trip.
  bool DirectResolveRenderTargets(
      const draw_util::ResolveInfo& resolve_info,
      const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
      uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
      uint32_t dump_pitch, bool copy_dest_scaled,
      VulkanSharedMemory& shared_memory, VulkanTextureCache& texture_cache);

  // Writes contents of host render targets within rectangles from
  // ResolveInfo::GetCopyEdramTileSpan to edram_buffer_ - with the plain 1x1
  // tile layout if native_layout is set.
  void DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                         uint32_t dump_rows, uint32_t dump_pitch,
                         bool native_layout);

  void DumpAllRenderTargetsToEdram() override;
  bool BeginEdramSnapshotReadback() override;
  const void* MapEdramSnapshotReadback() override;
  void EndEdramSnapshotReadback() override;

 public:
  void RestoreEdramSnapshot(const void* snapshot);

 private:
  bool gamma_render_target_as_unorm16_ = false;

  bool depth_unorm24_vulkan_format_supported_ = false;
  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_attachments_supported_ = false;
  bool msaa_2x_no_attachments_supported_ = false;

  // VK_NULL_HANDLE if failed to create.
  std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKey::Hasher>
      render_passes_;

  std::unordered_map<FramebufferKey, Framebuffer, FramebufferKey::Hasher>
      framebuffers_;

  // Set 0 - EDRAM storage buffer, set 1 - source depth sampled image (and
  // unused stencil from the transfer descriptor set), HostDepthStoreConstants
  // passed via push constants.
  VkPipelineLayout host_depth_store_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X) + 1] =
      {};

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool>
      transfer_vertex_buffer_pool_;
  VkShaderModule transfer_passthrough_vertex_shader_ = VK_NULL_HANDLE;
  VkPipelineLayout transfer_pipeline_layouts_[size_t(
      EdramTransferPipelineLayoutIndex::kCount)] = {};
  // VK_NULL_HANDLE if failed to create.
  std::unordered_map<EdramTransferShaderKey, VkShaderModule,
                     EdramTransferShaderKey::Hasher>
      transfer_shaders_;
  // With sample-rate shading, one pipeline per entry. Without sample-rate
  // shading, one pipeline per sample per entry. VK_NULL_HANDLE if failed to
  // create.
  std::unordered_map<TransferPipelineKey, std::array<VkPipeline, 4>,
                     TransferPipelineKey::Hasher>
      transfer_pipelines_;

  VkPipelineLayout dump_pipeline_layout_color_ = VK_NULL_HANDLE;
  VkPipelineLayout dump_pipeline_layout_depth_ = VK_NULL_HANDLE;
  // Compute pipelines for copying host render target contents to the EDRAM
  // buffer. VK_NULL_HANDLE if failed to create.
  std::unordered_map<EdramDumpShaderKey, VkPipeline, EdramDumpShaderKey::Hasher>
      dump_pipelines_;

  // Temporary storage for Resolve.
  std::vector<Transfer> clear_transfers_[2];

  // Temporary storage for PerformTransfersAndResolveClears.
  std::vector<TransferInvocation> current_transfer_invocations_;

  // Temporary storage for DumpRenderTargets.
  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  std::vector<DumpInvocation> dump_invocations_;

  // For pixel (fragment) shader interlock.

  VkRenderPass fsi_render_pass_ = VK_NULL_HANDLE;
  Framebuffer fsi_framebuffer_;

  VkPipelineLayout resolve_fsi_clear_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_32bpp_pipeline_ = VK_NULL_HANDLE;
  VkPipeline resolve_fsi_clear_64bpp_pipeline_ = VK_NULL_HANDLE;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_RENDER_TARGET_CACHE_H_
