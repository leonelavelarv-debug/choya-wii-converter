// Render de modelos del paquete con OpenGL ES 3 (TEV o sombreado simple).
// Lo comparten choya_model (consola, con SDL2) y choya_render (PNG sin pantalla, con EGL).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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
    int light_set_materials = 0;   // materiales con light_set_idx (usan las luces de la escena si hay una)
    int ref_matrices = 0;          // matrices de textura con ref_camera o ref_light
    int hidden_submeshes = 0;      // ocultas por defecto (nodo sin FLAG_VISIBLE)
    double ms = 0;
    bool ok = false;
};

// Diagnóstico de la escena en el último draw(): qué se resolvió con la escena y qué cayó al visor.
struct SceneDiag {
    bool scene = false;            // hay una escena SCN0 activa
    int lit_materials = 0;         // materiales con luces de la escena
    int viewer_light = 0;          // materiales con la luz del visor (sin escena o sin conjunto)
    int missing_set = 0;           // light_set_idx que la escena no tiene
    int over_limit = 0;            // conjuntos con más de kMaxLights luces encendidas (las demás no se dibujan)
    int ref_camera_scene = 0, ref_camera_fallback = 0;
    int ref_light_scene = 0, ref_light_fallback = 0;
    std::string text() const;      // una línea para el registro
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

    // Estado de material (colores, matrices de textura, texturas por unidad) y visibilidad de cada submalla. Por
    // cuadro: reset_material_state() y luego apply_mat_anim() de cada animación del grupo, con el mismo cuadro que la
    // CHR0. Así una animación nunca deja restos en la siguiente.
    void reset_material_state();
    void apply_mat_anim(const MatAnimView& a, uint32_t frame);
    // Sube de una vez las texturas que una PAT0 puede poner (cambiar de textura después no vuelve a subir nada).
    int prepare_textures(const MatAnimView& a);
    // Escena SCN0 (luces por light_set_idx, ref_camera / ref_light); nullptr = luz y cámara del visor.
    void set_scene(const SceneView* scene, uint32_t frame);
    const SceneDiag& scene_diag() const { return diag_; }
    bool submesh_visible(uint32_t s) const { return s < vis_.size() ? vis_[s] != 0 : true; }
    uint64_t map_key(uint32_t material, int unit) const;   // la textura que se usa ahora (para pruebas)

private:
    struct Program {
        uint32_t id = 0;
        int mvp = -1, mv = -1, light = -1, skinned = -1;
        int tex[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        int creg = -1, kreg = -1, chmat = -1, chamb = -1, tm = -1, tgm = -1;
        int lpos = -1, ldir = -1, lcol = -1, lcos = -1, ldist = -1, lmask = -1;
    };
    struct MatState {
        float creg[4][4], kreg[4][4], chmat[2][4], chamb[2][4], tm[8][8];
        uint64_t map_key[8];
    };
    struct LightUniforms {
        float pos[4][4], dir[4][4], col[4][4], cos[4][4], dist[4][4];
        int mask[4];       // COLOR0, ALPHA0, COLOR1, ALPHA1
        float amb[4];      // color de la luz ambiente del conjunto (0..255)
        bool valid = false;
    };
    void light_uniforms(int set_ref, const float view[16], LightUniforms& out);
    void viewer_lights(const TevMaterialRec& m, LightUniforms& out);
    void scene_matrix(const TevMaterialRec& m, int slot, const float view[16], const float proj[16], float out[16]);
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
    std::vector<MatState> base_, state_;
    std::vector<uint8_t> vis_base_, vis_;
    const SceneView* scene_ = nullptr;
    uint32_t scene_frame_ = 0;
    SceneDiag diag_;
    std::vector<LightUniforms> set_cache_;   // por índice de conjunto, se rehace en cada draw()
};

}  // namespace wl
