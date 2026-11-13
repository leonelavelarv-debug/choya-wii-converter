// Render de modelos del paquete con OpenGL ES 3 (TEV o sombreado simple).
// Lo comparten choya_model (consola, con SDL2) y choya_render (PNG sin pantalla, con EGL).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

#include "wl/model.h"
#include "wl/pack.h"
#include "wl/tev_gen.h"

namespace wl {

struct RenderOptions {
    bool tev = true;        // false: sombreado simple (textura principal x color x luz)
    bool cull = true;
    bool textures = true;   // solo en modo simple
    bool lighting = true;   // solo en modo simple
};

struct LoadInfo {
    std::string name;
    uint32_t vertices = 0, triangles = 0, submeshes = 0, materials = 0;
    int new_programs = 0, missing_textures = 0;
    double ms = 0;
    bool ok = false;
};

class ModelRenderer {
public:
    explicit ModelRenderer(const Pack& pack) : pack_(pack) {}
    ~ModelRenderer();
    bool init();                               // requiere un contexto GLES 3 activo
    LoadInfo load(uint32_t entry_index);       // índice de entrada del paquete
    void unload();
    // Matrices columna-mayor de 4x4. Dibuja opacos y luego translúcidos.
    void draw(const float view[16], const float proj[16], const RenderOptions& opt);
    const ModelView& view() const { return view_; }
    // Pose de los huesos: una matriz de piel 3x4 por hueso (AnimView::frame); nullptr = pose de reposo.
    void set_pose(const float* skin, uint32_t bones);
    int tev_failures() const { return tev_failures_; }

private:
    struct Program {
        uint32_t id = 0;
        int mvp = -1, mv = -1, light = -1, skinned = -1;
        int tex[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    };
    uint32_t texture(uint64_t key);
    Program build(const TevMaterialRec& m, const TevGenOptions& opt);

    const Pack& pack_;
    ModelView view_{};
    TevGenOptions gen_{};
    uint32_t vao_ = 0, vbo_ = 0, ibo_ = 0;
    uint32_t white_ = 0;
    uint32_t skin_ubo_ = 0;
    uint32_t simple_ = 0;
    int u_mvp_ = -1, u_tex_ = -1, u_use_tex_ = -1, u_vcol_ = -1, u_lit_ = -1, u_atest_ = -1, u_light_ = -1, u_skinned_ = -1;
    std::unordered_map<uint64_t, uint32_t> textures_;
    std::unordered_map<uint64_t, Program> programs_;
    int tev_failures_ = 0;
};

}  // namespace wl
