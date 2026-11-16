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
    // Modo con uniformes (el visor de modelos): registros de color, konstantes, colores de los canales, matrices de
    // textura, matrices de la escena (entorno/proyección, ref_camera/ref_light) y hasta kMaxLights luces de GX van en
    // uniformes. Las animaciones de material y las luces de la escena cambian valores, nunca el programa: un material
    // compila una vez. Sin este modo (wl_tevgen, pruebas) los valores van como constantes en el código.
    bool dynamic = false;
};
constexpr int kMaxLights = 4;  // luces por material en el visor (límite pensado para la Mali-G31 de la RG35XX H)

// Vertex shader común: atributos 0 pos, 1 normal, 2 color, 3 uv0. Uniformes: mvp, mv.
const char* tev_vertex_source();

// Fragment shader del material. Samplers "tex0".."tex7" = GX_TEXMAP0..7. Uniforme "light" (vista).
// Con opt.dynamic: uniformes u_creg[4], u_kreg[4], u_chmat[2], u_chamb[2], u_tm[16] (filas s y t de cada matriz de
// textura), u_tgm[8] (matriz de la escena por coordenada en los modos de entorno y proyección), y las luces u_lpos,
// u_ldir, u_lcol, u_lcos, u_ldist [kMaxLights] con u_lmask (máscara por canal: COLOR0, ALPHA0, COLOR1, ALPHA1).
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
