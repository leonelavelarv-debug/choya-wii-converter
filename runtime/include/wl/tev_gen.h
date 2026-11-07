// Generador de shaders: TEV de GameCube/Wii -> GLSL ES 3.00 para la Mali-G31.
//
// Traduce el estado TEV de un material (registros crudos del hardware GX) a un fragment
// shader. El mismo generador servirá para la capa GX del juego: allí el estado vendrá de las
// escrituras BP que haga el código del juego en vez de venir del paquete.
#pragma once
#include <cstdint>
#include <string>

#include "wl/model.h"

namespace wl {

struct TevGenOptions {
    bool has_normals = true;      // hay normales: se aproxima la iluminación del canal COLOR0
    bool has_vertex_color = false;
};

// Vertex shader común: atributos 0 pos, 1 normal, 2 color, 3 uv0. Uniformes: mvp, mv.
const char* tev_vertex_source();

// Fragment shader del material. Samplers "tex0".."tex7" = GX_TEXMAP0..7. Uniforme "light" (vista).
std::string tev_fragment_source(const TevMaterialRec& m, const TevGenOptions& opt);

// Estados de render del material traducidos a constantes de OpenGL ES.
struct GlBlendState {
    bool blend = false;
    bool subtract = false;
    uint32_t src = 0, dst = 0;    // GL_ONE, GL_SRC_ALPHA, ...
    bool depth_test = true;
    uint32_t depth_func = 0;      // GL_LEQUAL, ...
    bool depth_write = true;
    bool color_write = true;
    bool alpha_write = true;
};
GlBlendState tev_blend_state(const TevMaterialRec& m);

// Huella del material (sin claves de textura) para reutilizar programas compilados.
uint64_t tev_program_hash(const TevMaterialRec& m, const TevGenOptions& opt);

}  // namespace wl
