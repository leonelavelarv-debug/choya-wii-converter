// Choya Wii Converter: núcleo del motor GLES de Choya (independiente de aurora).
//
// Recibe cuadros del GX ya armados (vértices crudos, uniformes, índices, arreglos y una lista de pases y dibujos)
// con descripciones propias de cada pipeline, y los dibuja con OpenGL ES 3.2. No incluye nada de aurora ni de
// WebGPU: el adaptador (choya_gx.cpp) traduce las estructuras de aurora a estas. Gracias a eso, un cuadro real
// volcado en la consola (CHOYA_GX_VOLCADO) se puede volver a dibujar en la PC con Mesa para depurar.
#pragma once

#include <cstddef>
#include <cstdint>

namespace choya::gx {

// Todo en u8: sin relleno, mismo diseño en ARM64 y x86-64 (los volcados se leen en la PC).
struct AttrDesc {
  uint8_t attrType, cnt, compType, offset, stride, frac, le, pad;
};
struct ChanDesc {
  uint8_t matSrc, ambSrc, lightingEnabled, pad;
};
struct PipelineDesc {
  // --- lo que define el programa GLSL (se usa como clave de la caché de programas) ---
  AttrDesc attrs[26];  // por GXAttr
  ChanDesc chans[4];   // GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1
  uint8_t vtxStride, lineMode, sampledChannels, postexCount, nrmCount, tevRegLoads, lightingEnabled, pad0;
  // --- estado de render ---
  uint8_t depthCompare, depthUpdate, colorUpdate, alphaUpdate, depthFunc, cullMode, blendMode, blendSrc;
  uint8_t blendDst, blendOp, pixelFmt, pad1[5];
};
constexpr size_t kShaderKeyBytes = sizeof(AttrDesc) * 26 + sizeof(ChanDesc) * 4 + 8;
static_assert(sizeof(PipelineDesc) == kShaderKeyBytes + 16);

struct ClearDesc {
  uint8_t color, alpha, depth, pad;
};
struct Viewport {
  float left, top, width, height, znear, zfar;
};
struct Rect {
  int32_t x, y, width, height;
};

using LogFn = void (*)(const char* line);
using ProcFn = void* (*)(const char* name);
void set_logger(LogFn fn) noexcept;
void set_gl_loader(ProcFn fn) noexcept;
void set_reversed_z(bool reversed) noexcept;

void register_pipeline(uint64_t ref, const PipelineDesc& desc) noexcept;
bool has_pipeline(uint64_t ref) noexcept;
void register_clear(uint64_t ref, const ClearDesc& desc) noexcept;
bool has_clear(uint64_t ref) noexcept;

// Captura de un cuadro, en orden: capture_begin, pases con sus comandos, capture_end.
void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept;
void capture_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float clearRgba[4],
                  float clearDepthValue, bool hasResolve) noexcept;
void capture_viewport(const Viewport& vp) noexcept;
void capture_scissor(const Rect& rect) noexcept;
void capture_draw(uint64_t pipeline, uint32_t idxOffset, uint32_t indexCount, uint32_t instanceCount,
                  uint32_t uniformOffset, uint32_t dstAlpha) noexcept;
void capture_clear(uint64_t pipeline, const float rgba[4], float depth, bool useScissor, const Rect& rect) noexcept;
void capture_end() noexcept;
void capture_discard() noexcept;
bool capturing() noexcept;

// Dibuja el último cuadro capturado en el framebuffer activo (contexto GLES 3.2 ya activo).
bool draw_frame(int screenWidth, int screenHeight) noexcept;

// Volcado del último cuadro (datos + pipelines que usa) y su lectura en la PC.
bool dump_last_frame(const char* path) noexcept;
bool load_frame(const char* path) noexcept;

} // namespace choya::gx
