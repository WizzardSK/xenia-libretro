#ifndef XENIA_GPU_D3D12_SHADERS_XENOS_DRAW_HLSLI_
#define XENIA_GPU_D3D12_SHADERS_XENOS_DRAW_HLSLI_

// Only the fields the tessellation shaders read, pinned to the offsets
// SpirvShaderTranslator::SystemConstants puts them at - the same four
// xenos_draw.glsli declares, guarded by the static_assert next to them.
cbuffer xe_system_cbuffer : register(b0) {
  float2 xe_tessellation_factor_range : packoffset(c32.x);
  uint xe_vertex_index_endian : packoffset(c33.x);
  uint xe_vertex_index_offset : packoffset(c33.y);
  uint2 xe_vertex_index_min_max : packoffset(c33.z);
};

struct XeHSControlPointInputIndexed {
  float index : XEVERTEXID;
};

struct XeHSControlPointInputAdaptive {
  // 1.0 added in the vertex shader to convert to Direct3D 11+, and clamped to
  // the factor range in the vertex shader.
  float edge_factor : XETESSFACTOR;
};

struct XeHSControlPointOutput {
  float index : XEVERTEXID;
};

#endif  // XENIA_GPU_D3D12_SHADERS_XENOS_DRAW_HLSLI_
