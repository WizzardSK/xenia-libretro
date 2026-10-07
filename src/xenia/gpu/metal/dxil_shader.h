/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_DXIL_SHADER_H_
#define XENIA_GPU_METAL_DXIL_SHADER_H_

#include <cstdint>
#include <string>

#include "xenia/gpu/spirv_shader.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

class MetalShaderConverter;

// Metal shader translated via SPIR-V -> DXIL (Mesa) -> AIR (Metal Shader
// Converter). Resources follow the MSC top-level argument buffer ABI that
// MetalShaderConverter's root signature describes.
class DxilShader : public SpirvShader {
 public:
  DxilShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
             const uint32_t* ucode_dwords, size_t ucode_dword_count,
             std::endian ucode_source_endian = std::endian::big);

  class DxilTranslation : public SpirvTranslation {
   public:
    DxilTranslation(DxilShader& shader, uint64_t modification)
        : SpirvTranslation(shader, modification) {}
    ~DxilTranslation();

    // Converts the SPIR-V binary (already in translated_binary()) to DXIL and
    // then to a Metal library, and looks up its entry point.
    bool CompileToAir(MTL::Device* device,
                      const MetalShaderConverter& converter);

    MTL::Library* metal_library() const { return metal_library_; }
    MTL::Function* metal_function() const { return metal_function_; }
    const std::string& entry_point_name() const { return entry_point_name_; }

   private:
    MTL::Library* metal_library_ = nullptr;
    MTL::Function* metal_function_ = nullptr;
    std::string entry_point_name_;
  };

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_DXIL_SHADER_H_
