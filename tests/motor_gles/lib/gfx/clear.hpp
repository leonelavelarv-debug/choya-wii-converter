#pragma once
#include "common.hpp"
namespace wgpu { struct Color { double r, g, b, a; }; }
namespace aurora::gfx::clear {
struct DrawData { PipelineRef pipeline; Range uniformRange; wgpu::Color color; float depth = 0.f; bool useScissor = false; ClipRect scissor{}; };
struct PipelineConfig { uint32_t version = 3; uint32_t msaaSamples = 1; bool clearColor = true; bool clearAlpha = true; bool clearDepth = true; uint8_t _pad = 0; };
}
