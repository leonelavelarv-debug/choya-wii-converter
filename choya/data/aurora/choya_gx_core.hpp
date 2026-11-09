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
// Una etapa del TEV (valores de GXEnum). texCoord/texMap = 0xFF: la etapa no muestrea textura.
struct TevStageDesc {
  uint8_t ca, cb, cc, cd, cop, cbias, cscale, cclamp, cout;  // color: argumentos A-D, operación, salida
  uint8_t aa, ab, ac, ad, aop, abias, ascale, aclamp, aout;  // alfa
  uint8_t kcSel, kaSel, texCoord, texMap, channel, swapRas, swapTex, pad;
};
static_assert(sizeof(TevStageDesc) == 26);
// Generación de coordenadas de textura. mtxSlot: índice en postex (0xFF identidad, 0xFE índice por vértice).
struct TexGenDesc {
  uint8_t type, src, mtxSlot, pad;
};

struct PipelineDesc {
  // --- lo que define el programa GLSL (se usa como clave de la caché de programas) ---
  AttrDesc attrs[26];  // por GXAttr
  ChanDesc chans[4];   // GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1
  uint8_t vtxStride, lineMode, sampledChannels, postexCount, nrmCount, tevRegLoads, lightingEnabled, numTexGens;
  uint8_t tevStageCount, alphaComp0, alphaRef0, alphaOp, alphaComp1, alphaRef1, sampledTexCoords, pad0;
  TevStageDesc stages[16];
  uint8_t swap[4][4];  // tablas de intercambio: canal de origen de r, g, b, a
  TexGenDesc texgens[8];
  // Desplazamientos de los uniformes en palabras de 32 bits (0 = no está), en el orden de build_uniform.
  uint16_t offProj, offPostex, offTevReg[4], offKColor[4], offChanMat[2], offChanMatA[2], offTexScale[8],
      offTexSize[8];
  // --- estado de render ---
  uint8_t depthCompare, depthUpdate, colorUpdate, alphaUpdate, depthFunc, cullMode, blendMode, blendSrc;
  uint8_t blendDst, blendOp, pixelFmt, pad1[5];
};
constexpr size_t kShaderKeyBytes = offsetof(PipelineDesc, depthCompare);
static_assert(sizeof(PipelineDesc) == kShaderKeyBytes + 16);

// Muestreo de una textura (independiente de WebGPU/GL). wrap: 0 borde, 1 repetir, 2 espejo.
// filtros: 0 cercano, 1 lineal; mip: 0 sin mipmaps, 1 cercano, 2 lineal.
struct SamplerDesc {
  uint8_t wrapS, wrapT, minFilter, magFilter, mipFilter, pad[3];
  float minLod, maxLod;
};
// Lo que enlaza un grupo de texturas de aurora: id de textura por ranura (0 = ninguna) y su muestreo.
struct BindDesc {
  uint64_t tex[8];
  SamplerDesc samp[8];
};

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
void register_bind_group(uint64_t ref, const BindDesc& desc) noexcept;
bool has_bind_group(uint64_t ref) noexcept;

// Texturas ya decodificadas a RGBA8 (con su cadena de mipmaps); se suben a GL en el hilo del motor.
void texture_data(uint64_t id, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* rgba,
                  size_t size) noexcept;
void texture_released(uint64_t id) noexcept;
// Guardar copia de las texturas en memoria (solo para volcar cuadros).
void set_keep_textures(bool keep) noexcept;

// Captura de un cuadro, en orden: capture_begin, pases con sus comandos, capture_end.
void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept;
// hasResolve: el pase termina con la copia a pantalla; copyRect es la zona del EFB que se copia.
void capture_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float clearRgba[4],
                  float clearDepthValue, bool hasResolve, const Rect& copyRect) noexcept;
// Copia del EFB a una textura (GXCopyTex) al terminar el pase actual: la zona rect (coordenadas del pase) va a la
// textura texId (texW x texH) convertida al formato fmt (GXTexFmt); opaque pone el alfa a 1.
void capture_copy(uint64_t texId, const Rect& rect, uint32_t texW, uint32_t texH, uint32_t fmt, bool opaque) noexcept;
void capture_viewport(const Viewport& vp) noexcept;
void capture_scissor(const Rect& rect) noexcept;
void capture_draw(uint64_t pipeline, uint32_t idxOffset, uint32_t indexCount, uint32_t instanceCount,
                  uint32_t uniformOffset, uint32_t dstAlpha, uint64_t bindGroup) noexcept;
void capture_clear(uint64_t pipeline, const float rgba[4], float depth, bool useScissor, const Rect& rect) noexcept;
// present = false: lote partido a mitad de cuadro (staging lleno); se dibuja en el EFB sin mostrarse.
void capture_end(bool present = true) noexcept;
void capture_discard() noexcept;
bool capturing() noexcept;

// Dibuja el último cuadro capturado en el framebuffer activo (contexto GLES 3.2 ya activo).
bool draw_frame(int screenWidth, int screenHeight) noexcept;

// Volcado del último cuadro (datos + pipelines que usa) y su lectura en la PC.
bool dump_last_frame(const char* path) noexcept;
bool load_frame(const char* path) noexcept;

} // namespace choya::gx
