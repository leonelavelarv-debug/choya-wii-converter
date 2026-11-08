#pragma once
#include "../gfx/common.hpp"
#include <array>
#include <bitset>
using u8 = uint8_t; using u32 = uint32_t;
namespace GX { constexpr u8 MaxLights = 8; }
namespace aurora::gx {
constexpr bool UseReversedZ = true;
constexpr u32 MaxVtxAttr = 26, MaxTexCoord = 8, MaxTextures = 8, MaxKColors = 4, MaxColorChannels = 4, MaxTevRegs = 4,
  MaxPTTexMtx = 20, MaxIndStages = 4, MaxIndTexMtxs = 3, MaxPostexMtx = 20, MaxPnMtx = 10, MaxTevStages = 16;
struct ColorChannelConfig { GXColorSrc matSrc = GX_SRC_REG; GXColorSrc ambSrc = GX_SRC_REG; GXDiffuseFn diffFn = GX_DF_NONE; GXAttnFn attnFn = GX_AF_NONE; bool lightingEnabled = false; u8 _p1=0,_p2=0,_p3=0; };
struct AttrConfig { u8 attrType = GX_NONE; u8 cnt = 0xFF; u8 compType = 0xFF; u8 offset = 0; u8 stride = 0; u8 frac = 0; bool le = true; u8 nrmIndexCount = 0; };
struct ShaderConfig { u8 fogType = 0; u8 vtxStride = 0; u8 lineMode : 2 = 0; u8 dualTexEnabled : 1 = 0; u8 fogRangeAdjust : 1 = 0; u8 zFreeze : 1 = 0; u8 pad1 : 3 = 0; u8 numTexGens = 0; u32 zTexture = 0;
  std::array<AttrConfig, MaxVtxAttr> attrs; std::array<ColorChannelConfig, MaxColorChannels> colorChannels; u32 tevStageCount = 0; };
inline int shader_vertex_color_attr(const ShaderConfig& config, u32 channel) noexcept {
  const bool hasColor0 = config.attrs[GX_VA_CLR0].attrType != GX_NONE; const bool hasColor1 = config.attrs[GX_VA_CLR1].attrType != GX_NONE;
  if (channel == 0) { if (hasColor0) return GX_VA_CLR0; if (hasColor1) return GX_VA_CLR1; } else if (channel == 1 && hasColor0 && hasColor1) return GX_VA_CLR1; return -1; }
inline bool render_target_has_alpha(GXPixelFmt pixelFmt) noexcept { return pixelFmt == GX_PF_RGBA6_Z24; }
struct GXBindGroups { void* textureBindGroup; void* resolvedTextureBindGroup = nullptr; };
struct UniformMatrixLayout { std::array<u8, MaxPostexMtx> postexSlots{}; std::array<u8, MaxPostexMtx> postexRemap{}; std::array<u8, MaxPnMtx> nrmSlots{}; u8 postexCount = 0; u8 nrmCount = 0; bool absolutePosRegion = false; };
struct ShaderInfo { std::bitset<MaxTexCoord> sampledTexCoords; std::bitset<MaxColorChannels / 2> sampledColorChannels;
  std::bitset<MaxTevRegs> loadsTevRegRgb, loadsTevRegAlpha; UniformMatrixLayout matrixLayout; u32 uniformSize = 0; bool lightingEnabled = false; };
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
}
