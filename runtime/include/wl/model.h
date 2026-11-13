// Mallas del paquete (entrada KIND_MODEL, bloque "CMDL"; ver choya/model_pack.py).
#pragma once
#include <cstdint>
#include <vector>

#include "wl/pack.h"

namespace wl {

constexpr uint8_t KIND_MODEL = 3;
constexpr uint8_t KIND_ANIM = 4;  // animación de huesos horneada (bloque "CANM"; ver choya/anim.py)

#pragma pack(push, 1)
struct ModelVertexV2 {  // versiones 1 y 2: sin huesos
    float pos[3];
    float nrm[3];
    uint8_t col[4];
    float uv[2];
};
struct ModelVertex {    // versión 3
    float pos[3];
    float nrm[3];
    uint8_t col[4];
    float uv[2];
    uint8_t bone[4];    // índice en ModelView::bones
    uint8_t weight[4];  // suman 255; los cuatro en 0 = vértice fijo
};
struct ModelBone {
    int32_t parent;     // -1 = raíz
    uint32_t flags;
    float inv_rest[12]; // inversa de la pose de reposo, 3x4 por filas
    uint32_t name_hash; // FNV-1a de 32 bits del nombre
    uint32_t reserved;
};
struct ModelSubmesh {
    uint32_t first;
    uint32_t count;
    uint64_t texture_key;  // 0 = sin textura
    uint8_t cull;          // 0 ninguno, 1 frente, 2 atrás, 3 todo
    uint8_t xlu;           // translúcido: se dibuja al final con mezcla
    uint8_t wrap_s;        // 0 clamp, 1 repetir, 2 espejo
    uint8_t wrap_t;
    uint32_t material;     // índice en ModelView::materials (versión 2)
    uint8_t reserved[8];
};

// Material TEV (512 bytes; ver choya/model_pack.py). Registros en los mismos bits que el hardware GX.
struct TevStageRec {
    uint32_t color_env;
    uint32_t alpha_env;
    uint8_t texmap, texcoord, tex_on, ras;
};
struct TevMapRec {
    uint64_t texture_key;
    uint8_t wrap_s, wrap_t, present, pad[5];
};
// Canal de luz del material (ResChanData de NW4R): colores y el registro de control de GX (XF 0x100E).
struct ChanRec {
    uint32_t flags;       // 1 color de material, 2 alfa de material, 4 color ambiente, 8 alfa ambiente,
                          // 16 control del color, 32 control del alfa (lo que trae el material)
    uint8_t mat[4];
    uint8_t amb[4];
    uint32_t ctrl_color;  // bit 0 material del vértice, 1 luz encendida, 2-5 y 11-14 luces, 6 ambiente del vértice,
    uint32_t ctrl_alpha;  // 7-8 función difusa (0 ninguna, 1 con signo, 2 recortada), 9 atenuación
};
struct TevMaterialRec {
    uint8_t num_stages, num_texgens, xlu, cull;
    uint32_t alpha_compare, blend, zmode;
    int16_t cregs[4][4];  // PREV, C0, C1, C2 (RGBA, -1024..1023)
    uint8_t kregs[4][4];  // K0..K3
    uint32_t ksel[8];
    uint32_t texgen[8];
    uint32_t dualtex[8];
    TevStageRec stages[16];
    TevMapRec maps[8];
    uint8_t pad[32];
    // Versión 3:
    float tex_mtx[8][2][4];   // matriz de textura por coordenada (filas s y t sobre (s, t, 1, 1))
    uint8_t tex_mtx_mode[8];  // 0 sin matriz; 1 + modo de mapeo de NW4R (0 coordenadas, 1/3/4 entorno, 2 proyección)
    uint8_t num_chans, pad2[3];
    ChanRec chans[2];
    uint8_t pad3[204];
};
#pragma pack(pop)
static_assert(sizeof(ModelVertexV2) == 36, "vértice de 36 bytes");
static_assert(sizeof(ModelVertex) == 44, "vértice de 44 bytes");
static_assert(sizeof(ModelBone) == 64, "hueso de 64 bytes");
static_assert(sizeof(ModelSubmesh) == 32, "submalla de 32 bytes");
static_assert(sizeof(TevStageRec) == 12, "etapa de 12 bytes");
static_assert(sizeof(ChanRec) == 20, "canal de 20 bytes");
static_assert(sizeof(TevMaterialRec) == 1024, "material de 1024 bytes");
constexpr size_t kMaterialV2Size = 512;

enum ModelFlags : uint32_t { MODEL_HAS_NORMALS = 1, MODEL_HAS_COLORS = 2, MODEL_SKINNED = 4 };

struct ModelView {
    const ModelVertex* vertices = nullptr;  // versión 3; las anteriores se copian a este formato (owned_vertices)
    uint32_t vertex_count = 0;
    const uint32_t* indices = nullptr;
    uint32_t index_count = 0;
    const ModelSubmesh* submeshes = nullptr;
    uint32_t submesh_count = 0;
    const TevMaterialRec* materials = nullptr;  // nullptr en paquetes de versión 1
    uint32_t material_count = 0;
    uint32_t version = 0;
    uint32_t flags = 0;
    float bbox_min[3] = {0, 0, 0};
    float bbox_max[3] = {0, 0, 0};
    const ModelBone* bones = nullptr;
    uint32_t bone_count = 0;
    std::vector<ModelVertex> owned_vertices;     // solo para paquetes de versión 1 y 2
    std::vector<TevMaterialRec> owned_materials;
};

// Interpreta una entrada de modelo (sin copiar datos en la versión 3). false si está dañada.
bool model_view(const Pack& pack, const PackEntry& e, ModelView& out);

// Animación horneada para un modelo: por cuadro, una matriz 3x4 por hueso (la de piel: pose animada por la inversa
// de la de reposo), lista para el vértice.
struct AnimView {
    uint64_t model_key = 0;
    uint32_t frames = 0, bones = 0;
    bool loop = false;
    float fps = 60.0f;
    const float* matrices = nullptr;  // frames * bones * 12
    const float* frame(uint32_t f) const { return matrices + size_t(f % (frames ? frames : 1)) * bones * 12; }
};
bool anim_view(const Pack& pack, const PackEntry& e, AnimView& out);
// Entradas KIND_ANIM horneadas para el modelo de esa clave (índices del paquete).
std::vector<uint32_t> anims_for(const Pack& pack, uint64_t model_key);

}  // namespace wl
