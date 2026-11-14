// Choya Wii Converter: adaptador entre aurora y el motor GLES de Choya (choya_gx_core).
//
// aurora sigue leyendo el GX del juego y armando cada cuadro (pases, dibujos, vértices, uniformes); aquí se copia
// ese cuadro al final de cada lote, traduciendo las estructuras de aurora a las del núcleo de Choya, y el motor de
// Choya lo dibuja con OpenGL ES 3.2 en su propia pantalla, en vez de Dawn.
//
// Solo se activa con CHOYA_GLES=1. Sin la variable, estas funciones no hacen nada.
#pragma once

#include "../gfx/clear.hpp"
#include "../gfx/common.hpp"
#include "../gx/pipeline.hpp"

#include <cstddef>
#include <cstdint>

namespace aurora::gfx {
struct TextureRef;
}

namespace aurora::choya {

bool enabled() noexcept;
// Con el motor encendido, Dawn (backend Null) no codifica los dibujos salvo con CHOYA_DAWN_DIBUJA=1 (0.20.3).
bool dawn_draws() noexcept;
// Sin GPU (0.22.0): con el motor encendido y Dawn sin dibujar, aurora no codifica pases ni presenta, no arma WGSL
// para sus pipelines ni guarda la base de datos de pipelines. Con el WebGPU nulo de Choya (sin_dawn) no hay Dawn.
inline bool sin_gpu() noexcept { return enabled() && !dawn_draws(); }

// Texturas: datos ya decodificados (RGBA8) al crearse o escribirse, y aviso cuando se liberan.
void texture_uploaded(const gfx::TextureRef& ref, const uint8_t* data, size_t size) noexcept;
void texture_released(const void* ref) noexcept;
// Textura estática del juego: si el paquete de recursos convertidos (CHOYA_PAQUETE) la tiene, devuelve una textura de
// aurora vacía que el motor de Choya dibuja con la versión convertida (aurora ya no la decodifica). Si no, vacío.
gfx::TextureHandle packed_static_texture(uint32_t width, uint32_t height, uint32_t mips, uint32_t format,
                                         const uint8_t* data, size_t size, const char* label) noexcept;
// Con paleta (0.21.0): la imagen y la paleta que aurora iba a combinar; si el paquete tiene esa pareja (misma clave
// que el conversor), el motor usa la versión convertida. Si no, {} y aurora sigue como siempre.
gfx::TextureHandle packed_palette_texture(uint32_t width, uint32_t height, uint32_t mips, uint32_t format,
                                          const uint8_t* data, size_t size, uint32_t tlutFormat, uint32_t tlutEntries,
                                          const uint8_t* tlut) noexcept;
// Grupo de texturas recién creado por gx (ranuras que muestrea el shader y su muestreo).
void register_texture_bind_group(gfx::BindGroupRef ref, const gx::ShaderInfo& info) noexcept;

void register_gx_pipeline(gfx::PipelineRef ref, const gx::PipelineConfig& config) noexcept;
void register_clear_pipeline(gfx::PipelineRef ref, const gfx::clear::PipelineConfig& config) noexcept;

void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept;
// isDisplayCopy: el pase termina con la copia a pantalla (GXCopyDisp); copyRect es la zona del EFB copiada.
void capture_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float clearRgba[4],
                  float clearDepthValue, bool isDisplayCopy, const gfx::ClipRect& copyRect) noexcept;
// Copia del EFB a una textura (GXCopyTex) al terminar el pase: rect es la zona del EFB, fmt el formato de la copia.
void capture_copy(const gfx::TextureRef* tex, const gfx::ClipRect& rect, uint32_t fmt, bool opaque) noexcept;
// Paleta sobre una copia del EFB (tex_palette_conv): variant 0 directo, 1 R*255 (C8), 2 R*15 (C4).
void capture_palette_conv(int variant, const gfx::TextureRef* src, const gfx::TextureRef* dst,
                          const gfx::TextureRef* tlut) noexcept;
void capture_viewport(const gfx::Viewport& vp) noexcept;
void capture_scissor(const gfx::ClipRect& rect) noexcept;
void capture_gx_draw(const gx::DrawData& draw) noexcept;
void capture_clear(const gfx::clear::DrawData& draw) noexcept;
void capture_end(bool present = true) noexcept;
void capture_discard() noexcept;

} // namespace aurora::choya

// Llamado por la pantalla de Choya (runtime) una vez por cuadro, con el contexto GLES ya activo.
// Devuelve false si todavía no hay un cuadro capturado que dibujar.
extern "C" bool choya_gx_draw_frame(int screenWidth, int screenHeight);
