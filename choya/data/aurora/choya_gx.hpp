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

namespace aurora::choya {

bool enabled() noexcept;

void register_gx_pipeline(gfx::PipelineRef ref, const gx::PipelineConfig& config) noexcept;
void register_clear_pipeline(gfx::PipelineRef ref, const gfx::clear::PipelineConfig& config) noexcept;

void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept;
// isDisplayCopy: el pase termina con la copia a pantalla (GXCopyDisp).
void capture_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float clearRgba[4],
                  float clearDepthValue, bool isDisplayCopy) noexcept;
void capture_viewport(const gfx::Viewport& vp) noexcept;
void capture_scissor(const gfx::ClipRect& rect) noexcept;
void capture_gx_draw(const gx::DrawData& draw) noexcept;
void capture_clear(const gfx::clear::DrawData& draw) noexcept;
void capture_end() noexcept;
void capture_discard() noexcept;

} // namespace aurora::choya

// Llamado por la pantalla de Choya (runtime) una vez por cuadro, con el contexto GLES ya activo.
// Devuelve false si todavía no hay un cuadro capturado que dibujar.
extern "C" bool choya_gx_draw_frame(int screenWidth, int screenHeight);
