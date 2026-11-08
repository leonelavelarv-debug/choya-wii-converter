#pragma once
#include "gx.hpp"
namespace aurora::gx {
struct DrawData { gfx::PipelineRef pipeline; gfx::Range vertRange, idxRange, uniformRange; uint32_t vtxCount, indexCount, instanceCount; bool expandedPrimitive; GXBindGroups bindGroups; uint32_t dstAlpha; };
struct PipelineConfig { uint32_t version = 20; uint32_t msaaSamples = 1; ShaderConfig shaderConfig; GXCompare depthFunc; GXCullMode cullMode; GXBlendMode blendMode; GXBlendFactor blendFacSrc, blendFacDst; GXLogicOp blendOp; GXPixelFmt pixelFmt; uint32_t dstAlpha; bool depthCompare, depthUpdate, alphaUpdate, colorUpdate; };
}
