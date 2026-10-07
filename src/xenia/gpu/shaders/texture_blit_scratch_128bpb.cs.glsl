/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Moves one mip level from the texture load scratch buffer into a staging
// image, replacing vkCmdCopyBufferToImage. NVIDIA's buffer-to-image path runs at
// about an eighth of the rate a shader reaches writing the same bytes (57 GB/s
// against 427 measured by xenia-vulkan-copy-bench at 4 bytes a texel), and the
// copy is 133 MB per
// frame at 3x resolution. An image-to-image copy then moves the result into the
// texture, which stays a plain sampled image and keeps its compression.
//
// Vulkan-only on purpose - Direct3D 12's CopyTextureRegion already runs at full
// copy bandwidth, so this lives in GLSL rather than the shared xesl dialect.
//
// One texel per host block, so nothing here interprets the blocks: the same
// shader serves colour and block-compressed destinations of that block size.
// Levels are stacked down the staging image (dest_offset_y) so a whole mip
// chain dispatches without barriers between levels, and the array layer is the
// dispatch's z.

#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeTextureBlitScratchConstants {
  // Offset of the level within the scratch buffer, in host blocks.
  uint offset_blocks;
  // Host blocks per row in the scratch buffer.
  uint pitch_blocks;
  // Host blocks per array slice in the scratch buffer.
  uint slice_blocks;
  // Row of the staging image this level starts at.
  uint dest_offset_y;
  // Extent of this level in host blocks.
  uint size_x;
  uint size_y;
} xe_texture_blit_scratch;

layout(set = 0, binding = 0, std430) readonly buffer XeTextureBlitScratchSource {
  uvec4 xe_texture_blit_scratch_source[];
};

layout(set = 0, binding = 1, rgba32ui) uniform writeonly uimage2DArray
    xe_texture_blit_scratch_dest;

void main() {
  uvec3 block_index = gl_GlobalInvocationID;
  if (block_index.x >= xe_texture_blit_scratch.size_x ||
      block_index.y >= xe_texture_blit_scratch.size_y) {
    return;
  }
  uvec4 value = xe_texture_blit_scratch_source[
      xe_texture_blit_scratch.offset_blocks +
      xe_texture_blit_scratch.slice_blocks * block_index.z +
      xe_texture_blit_scratch.pitch_blocks * block_index.y + block_index.x];
  imageStore(xe_texture_blit_scratch_dest,
             ivec3(int(block_index.x),
                   int(xe_texture_blit_scratch.dest_offset_y + block_index.y),
                   int(block_index.z)),
             value);
}
