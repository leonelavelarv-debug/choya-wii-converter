// Choya Wii Converter: motor GLES de Choya dentro de aurora (paso 2a).
//
// aurora sigue leyendo el GX del juego y armando cada cuadro (pases, dibujos, vértices, uniformes); aquí se copia
// ese cuadro al final de cada lote y el motor de Choya lo dibuja con OpenGL ES 3.2 en su propia pantalla, en vez
// de Dawn. En el paso 2a se dibuja la geometría con el color del rasterizador (sin texturas ni TEV todavía).
//
// Solo se activa con CHOYA_GLES=1. Sin la variable, estas funciones no hacen nada.
#pragma once

#include "../gfx/common.hpp"
#include "../gfx/clear.hpp"
#include "../gx/pipeline.hpp"

#include <cstddef>
#include <cstdint>

namespace aurora::choya {

bool enabled() noexcept;

// Configuraciones de cada PipelineRef (el hash con el que aurora nombra sus pipelines).
void register_gx_pipeline(gfx::PipelineRef ref, const gx::PipelineConfig& config) noexcept;
void register_clear_pipeline(gfx::PipelineRef ref, const gfx::clear::PipelineConfig& config) noexcept;

// Captura de un cuadro, en orden: begin_frame, pases con sus comandos, end_frame.
void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept;
void capture_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float clearRgba[4],
                  float clearDepthValue) noexcept;
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
