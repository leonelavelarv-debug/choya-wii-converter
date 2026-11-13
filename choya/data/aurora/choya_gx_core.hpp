// Choya Wii Converter: núcleo del motor GLES de Choya (independiente de aurora).
//
// Recibe cuadros del GX ya armados (vértices crudos, uniformes, índices, arreglos y una lista de pases y dibujos)
// con descripciones propias de cada pipeline, y los dibuja con OpenGL ES 3.2. No incluye nada de aurora ni de
// WebGPU: el adaptador (choya_gx.cpp) traduce las estructuras de aurora a estas. Gracias a eso, un cuadro real
// volcado en la consola (CHOYA_GX_VOLCADO) se puede volver a dibujar en la PC con Mesa para depurar.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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
  // --- TEV exacto (0.20.9): lo que faltaba para hacer el mismo programa que aurora. valid = 0 en volcados viejos
  // (el motor dibuja entonces como antes). También es parte de la clave del programa. Con valid = 1, el byte pad de
  // cada TevStageDesc es la coordenada de textura efectiva de la etapa (0xFF ninguna) y attrs[GX_VA_NRM].pad es
  // nrmIndexCount (normales NBT con tres índices).
  struct IndStageDesc {
    uint8_t texCoord, texMap, scaleS, scaleT;  // texCoord efectiva (0xFF ninguna)
  };
  // Indirecta de cada etapa. flags: 1 suma la anterior, 2 lleva coordenada fija, 4 la combinación usa la textura.
  struct TevIndDesc {
    uint8_t indStage, format, biasSel, alphaSel, mtxId, wrapS, wrapT, flags;
  };
  struct Ext {
    uint8_t valid, dualTex, fogType, fogRange, numIndStages, zFreeze, zTexStage, usedIndStages;
    uint8_t tgFlags[8];  // 1 normalizar (con dualTex), 2 entrada AB11
    uint8_t tgPost[8];   // matriz posterior 0..19 (GX_PTTEXMTXn), 0xFF identidad
    IndStageDesc ind[4];
    TevIndDesc tind[16];
    uint32_t zTexture;  // sesgo[0:23], formato[24:25], operación[26:27]
    uint16_t offPostMtx, offFog, offIndMtx, offZFreeze;
  } ext;
};
constexpr size_t kShaderKeyBytes = offsetof(PipelineDesc, depthCompare);
constexpr size_t kPipelineDescV4Bytes = offsetof(PipelineDesc, ext);  // tamaño en volcados hasta la 0.20.8
static_assert(kPipelineDescV4Bytes == kShaderKeyBytes + 16);
static_assert(sizeof(PipelineDesc::Ext) == 180);
static_assert(sizeof(PipelineDesc) == kPipelineDescV4Bytes + 180);

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
// Despierta al hilo que dibuja (el de la pantalla) cuando hay trabajo que no viene con un cuadro mostrado: lotes de
// una pantalla de carga, o la cola llena (0.20.7). Sin esto el motor solo dibujaba al presentar el juego un cuadro.
using WakeFn = void (*)();
void set_wake(WakeFn fn) noexcept;
// Depuración en la PC: el dibujo número index (en orden del cuadro) no se hace; -1 los hace todos.
void debug_skip_draw(int index) noexcept;
// Depuración en la PC: los píxeles RGBA8 de una copia del EFB ya hecha (filas desde la de abajo de GL).
bool debug_read_copy(uint64_t id, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) noexcept;
// Desde el inicio: grupos de lotes sin mostrarse, esperas con la cola llena, copias únicas perdidas (pruebas).
void queue_stats(uint64_t out[3]) noexcept;
// ¿El motor ya hizo esa copia del EFB? (solo desde el hilo que dibuja; pruebas)
bool has_copy_texture(uint64_t id) noexcept;
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
// Paquete de recursos convertidos por el conversor de Choya (assets.wlpk). pack_open lo abre una vez;
// pack_lookup busca la textura GX (datos crudos del juego, nivel 0 incluido) por la misma clave que el conversor
// (-1: no está); texture_from_pack hace que el motor use esa versión convertida para el id (aurora ya no la decodifica).
bool pack_open(const char* path) noexcept;
bool pack_is_open() noexcept;
int64_t pack_lookup(uint32_t gxFormat, uint32_t width, uint32_t height, const uint8_t* level0, size_t size) noexcept;
// Con paleta (C4/C8/C14X2, 0.21.0): la clave sigue con los bytes de la paleta y su formato (como el conversor).
int64_t pack_lookup_palette(uint32_t gxFormat, uint32_t width, uint32_t height, const uint8_t* level0, size_t size,
                            const uint8_t* tlut, size_t tlutBytes, uint32_t tlutFormat) noexcept;
bool texture_from_pack(uint64_t id, int64_t index) noexcept;
// Una textura en un formato del paquete (volcados y pruebas): format = formato de salida del conversor (1..10).
void texture_packed_data(uint64_t id, uint32_t width, uint32_t height, uint32_t mips, uint8_t format, uint8_t flags,
                         const uint8_t* data, size_t size) noexcept;
size_t gx_level_size(uint32_t gxFormat, uint32_t width, uint32_t height) noexcept;
uint64_t texture_key(const uint8_t* level0, size_t size, uint32_t width, uint32_t height, uint32_t gxFormat) noexcept;
// Fija el hilo que la llama (el del juego) al último núcleo y el resto del proceso a los demás (CHOYA_NUCLEOS=0 no).
void pin_game_thread() noexcept;
// Tiempos de aurora fuera del motor (0.20.2), en ns, para el informe cada 10 s: 0 el juego esperando a que el hilo
// de aurora suelte el cuadro anterior, 1 sellar, 2 codificar, 3 publicar, 4 esperar permiso para preparar, 5 preparar
// el siguiente, 6 listas de dibujo (GXCallDisplayList), 7 cuántas listas, 8 bytes de listas, 9 ciclos (cuadros).
void note_aurora(int what, uint64_t value) noexcept;
// Qué es cada textura que aparece en un grupo (para nombrar en el registro las que se dibujan sin datos, 0.20.4).
void describe_texture(uint64_t id, uint32_t width, uint32_t height, uint32_t gxFormat, uint32_t mips) noexcept;
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
// Paleta sobre una copia del EFB (C4/C8 + TLUT; tex_palette_conv de aurora), antes del pase que la usa: cada texel
// de dst = tlut[índice leído del canal R de src]. variant: 0 directo (índices enteros, no se usa), 1 R*255, 2 R*15.
void capture_palette_conv(uint64_t src, uint64_t dst, uint64_t tlut, uint32_t variant, uint32_t dstW,
                          uint32_t dstH) noexcept;
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
